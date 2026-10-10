# bbport: Windows build of the HLE runtime.
#
# The Linux build produces one executable (out/bb-probe) that links src/runtime*.c and reaches
# the GPU library through -rdynamic / --allow-shlib-undefined. A PE DLL cannot carry unresolved
# externals and an EXE is not a module other modules can import from by name, so the split is:
#
#   bbport_runtime.dll   src/probe.c (the ELF loader) + the HLE services. Exports runtime_*.
#   libbbgpu.dll         imports runtime_*, exports bbgpu_*   (gpu/CMakeLists.txt)
#   bb-probe.exe         locates the DLL next to itself and calls bbport_main.
#
# Build with -DBBPORT_GPU_LIB=<path to libbbgpu.dll.a>, or leave it unset to build the runtime
# first and reconfigure the GPU project afterwards (see scripts/build-win.sh).
cmake_minimum_required(VERSION 3.24)
project(bbport_runtime LANGUAGES C)

if (NOT WIN32)
    message(FATAL_ERROR "This CMakeLists builds the Windows HLE runtime only; use build.sh on Linux.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(BBPORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(BBPORT_SRC "${CMAKE_CURRENT_SOURCE_DIR}")

# Third-party ATRAC9 decoder: compiled without this project's -Werror policy, as build.sh does.
file(GLOB ATRAC9_SOURCES "${BBPORT_ROOT}/third_party/LibAtrac9/C/src/*.c")

set(RUNTIME_SOURCES
    "${BBPORT_SRC}/probe.c"
    "${BBPORT_SRC}/runtime.c"
    "${BBPORT_SRC}/runtime_win.c"
    "${BBPORT_SRC}/runtime_memory.c"
    "${BBPORT_SRC}/runtime_mutex.c"
    "${BBPORT_SRC}/runtime_thread.c"
    "${BBPORT_SRC}/runtime_content.c"
    "${BBPORT_SRC}/runtime_time.c"
    "${BBPORT_SRC}/runtime_sema.c"
    "${BBPORT_SRC}/runtime_rwlock.c"
    "${BBPORT_SRC}/runtime_kernel.c"
    "${BBPORT_SRC}/runtime_services.c"
    "${BBPORT_SRC}/runtime_zlib.c"
    "${BBPORT_SRC}/runtime_ajm.c"
    "${BBPORT_SRC}/runtime_audio.c"
    "${BBPORT_SRC}/runtime_pad.c"
    "${BBPORT_SRC}/runtime_rtc.c"
    "${BBPORT_SRC}/runtime_savedata.c"
    "${BBPORT_SRC}/runtime_file.c"
    "${BBPORT_SRC}/vulkan_smoke.c")

add_library(bbport_runtime SHARED ${RUNTIME_SOURCES} ${ATRAC9_SOURCES})
# No "lib" prefix: libbbgpu.dll names this module in its import table, and bb-probe.exe looks for
# it next to itself, so the on-disk name has to match on both sides.
set_target_properties(bbport_runtime PROPERTIES PREFIX "" OUTPUT_NAME "bbport_runtime")

target_include_directories(bbport_runtime PRIVATE
    "${BBPORT_ROOT}"
    "${BBPORT_ROOT}/gpu"
    "${BBPORT_ROOT}/gpu/shim")

# bcrypt for BCryptGenRandom (win_compat.h's getrandom); advapi32 for the crypto calls in
# runtime_kernel.c's guest_sysctl path; ws2_32 for runtime_services.c's sceNet byte-order and
# address conversions.
target_link_libraries(bbport_runtime PRIVATE bcrypt advapi32 ws2_32)

target_compile_definitions(bbport_runtime PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00)

# The loader and the guest fault handlers read guest code with GCC builtins and inline asm that
# assume the System V ABI and Unix signals.
target_compile_options(bbport_runtime PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# The guest memory backend reserves the fixed PS4 user range [0x1000000000,0xfc00000000) with a
# VirtualAlloc2 placeholder. High-entropy 64-bit ASLR would let the image be based anywhere in
# that range and make the reservation fail, so the module must be linked without it.
target_link_options(bbport_runtime PRIVATE "-Wl,--disable-high-entropy-va")

# Restrict the export table to runtime_* plus bbport_main. Passing a .def also turns off MinGW's
# auto-export of every global, which would leak the whole loader and fail on C++ COMDATs.
target_link_options(bbport_runtime PRIVATE "${BBPORT_SRC}/runtime_exports.def")

if (NOT BBPORT_VULKAN_LIB)
    find_library(BBPORT_VULKAN_LIB NAMES vulkan-1 libvulkan-1 vulkan
                 HINTS "$ENV{MSYSTEM_ROOT}/ucrt64/lib" "$ENV{MSYSTEM}/ucrt64/lib" REQUIRED)
endif()
# SDL3 backs runtime_audio.c (AJM output) and runtime_pad.c (controller input); zlib backs
# runtime_zlib.c. MSYS2 ships the MinGW import libraries next to the DLLs.
if (NOT BBPORT_SDL_LIB)
    find_library(BBPORT_SDL_LIB NAMES SDL3 SDL3-static
                 HINTS "$ENV{MSYSTEM_ROOT}/ucrt64/lib" "$ENV{MSYSTEM}/ucrt64/lib" REQUIRED)
endif()
if (NOT BBPORT_ZLIB_LIB)
    find_library(BBPORT_ZLIB_LIB NAMES z zlib
                 HINTS "$ENV{MSYSTEM_ROOT}/ucrt64/lib" "$ENV{MSYSTEM}/ucrt64/lib" REQUIRED)
endif()
if (BBPORT_GPU_LIB)
    target_link_libraries(bbport_runtime PRIVATE "${BBPORT_GPU_LIB}")
    # MSYS2 ships the loader as libvulkan-1.dll.a; there is no libvulkan.so symlink for -lvulkan.
    target_link_libraries(bbport_runtime PRIVATE "${BBPORT_VULKAN_LIB}")
endif()
# SDL3 and zlib are needed to link the DLL itself, independently of the GPU import library.
target_link_libraries(bbport_runtime PRIVATE "${BBPORT_SDL_LIB}" "${BBPORT_ZLIB_LIB}")

add_executable(bb-probe "${BBPORT_SRC}/launcher_win.c")
target_link_options(bb-probe PRIVATE "-static-libgcc")
# The launcher must not carry its own copy of runtime DLLs; it only uses kernel32.
target_link_libraries(bb-probe PRIVATE kernel32)

add_custom_target(hle ALL DEPENDS bbport_runtime bb-probe)