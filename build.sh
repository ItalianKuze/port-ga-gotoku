#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
# PGG_BUILD_DIR: the build folder (default out). PGG_RELEASE=1: a build to package (packaging/linux):
# only $ORIGIN library paths and no source paths in the binaries. PGG_GPU_CMAKE_ARGS: extra CMake
# arguments for the GPU library (packaging/linux passes the static libstdc++ there).
out=${PGG_BUILD_DIR:-out}
release=${PGG_RELEASE:-}
mkdir -p "$out"
CC=${CC:-$(command -v cc || command -v gcc || true)}
if [[ -z $CC ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
for tool in pkg-config cmake ninja git; do
    command -v "$tool" >/dev/null || { echo "Need $tool (see docs/BUILDING.md)." >&2; exit 1; }
done
pkg-config --exists vulkan || { echo 'Need the Vulkan loader and headers (pkg-config vulkan).' >&2; exit 1; }
# SDL3 from pkg-config; when the system has none (Ubuntu 24.04), or with PGG_SDL3_SOURCE=1, the
# release below is built from source into $out/deps/sdl3 (the same version as the Windows build).
SDL3_RELEASE=release-3.4.18
sdl3=$PWD/$out/deps/sdl3
if [[ ${PGG_SDL3_SOURCE:-} == 1 ]] || ! pkg-config --exists sdl3; then
    if [[ ! -f $sdl3/.$SDL3_RELEASE ]]; then
        echo "SDL3: building $SDL3_RELEASE from source into $sdl3"
        rm -rf "$out/deps/SDL" "$sdl3"
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$SDL3_RELEASE" https://github.com/libsdl-org/SDL.git "$out/deps/SDL"
        cmake -S "$out/deps/SDL" -B "$out/deps/SDL/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$sdl3" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF \
            -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >/dev/null
        ninja -C "$out/deps/SDL/build" install >/dev/null
        touch "$sdl3/.$SDL3_RELEASE"
    fi
    export PKG_CONFIG_PATH=$sdl3/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
fi
echo "SDL3 $(pkg-config --modversion sdl3) ($(pkg-config --variable=libdir sdl3))"
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# PGG_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. PGG_LTO=OFF disables link-time optimization.
pgo=${PGG_PGO:-${BB_PGO:-}}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init).
if [[ ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# The GPU library finds the same SDL3 as bb-probe ($out/deps/sdl3 when built from source). A
# release build finds its libraries next to it ($ORIGIN/../lib from gpu/), carries libstdc++ and
# libgcc inside (no newer libstdc++ needed on the player's system) and names no build paths
# (\$ORIGIN: the link command goes through a shell).
if [[ $release == 1 ]]; then
    prefix_map="-ffile-prefix-map=$PWD=."
    gpu_flags=(-DCMAKE_SKIP_BUILD_RPATH=ON -DCMAKE_C_FLAGS="$prefix_map" -DCMAKE_CXX_FLAGS="$prefix_map"
               '-DCMAKE_SHARED_LINKER_FLAGS=-static-libstdc++ -static-libgcc -Wl,-rpath,\$ORIGIN/../lib')
else
    prefix_map=
    gpu_flags=(-DCMAKE_BUILD_RPATH="$(pkg-config --variable=libdir sdl3)")
fi
read -r -a gpu_extra <<< "${PGG_GPU_CMAKE_ARGS:-}"
cmake -S gpu -B "$out/gpu" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${PGG_LTO:-${BB_LTO:-ON}}" -DBB_PGO_DIR="$PWD/pgo" \
    "${gpu_flags[@]}" "${gpu_extra[@]}" >/dev/null
echo "GPU library: PGO $pgo, LTO ${PGG_LTO:-${BB_LTO:-ON}}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C "$out/gpu" bbgpu > "$out/gpu-build.log" 2>&1; then
    grep -v '^\[' "$out/gpu-build.log" | tail -40 >&2
    echo "GPU library build failed (full log: $out/gpu-build.log)" >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it; the
# libraries a package brings along are in $ORIGIN/lib (an SDL3 built from source, else in $out/deps).
gpu=(-L"$out/gpu" -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -rdynamic)
libraries+=(-Wl,-rpath,'$ORIGIN/lib')
if [[ $release == 1 ]]; then
    libraries+=(-static-libgcc)
else
    gpu+=(-Wl,-rpath,"$PWD/$out/gpu")
    libraries+=(-Wl,-rpath,"$(pkg-config --variable=libdir sdl3)")
fi
cflags=(-O2 -g ${prefix_map:+"$prefix_map"})
# Runtime sources for this (Linux) build: runtime_win.c and any win32_*.c are the Windows backend.
runtime=()
for source in src/runtime*.c; do
    case $(basename "$source") in runtime_win.c|win32_*.c) ;; *) runtime+=("$source") ;; esac
done
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f $out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer "$out/libatrac9.a" -name '*.c') ]]; then
    rm -rf "$out/atrac9" && mkdir -p "$out/atrac9"
    for source in "${atrac9[@]}"; do "$CC" -std=c99 "${cflags[@]}" -w -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs "$out/libatrac9.a" "$out"/atrac9/*.o
fi
"$CC" -std=c11 "${cflags[@]}" -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc src/probe.c src/import_trace.c "${runtime[@]}" src/vulkan_smoke.c "$out/libatrac9.a" -lm -lz -ldl "${gpu[@]}" "${libraries[@]}" -o "$out"/bb-probe
echo "Built $PWD/$out/bb-probe"
# The launcher's gamepad list (--gamepads) and control capture (--read-input).
"$CC" -std=c11 "${cflags[@]}" -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o "$out"/bb-gpu-capabilities
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 "${cflags[@]}" -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -o "$out"/pad-test
    "$out"/pad-test
    "$CC" -std=c11 "${cflags[@]}" -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" "$out/libatrac9.a" -lm -lz -ldl "${gpu[@]}" "${libraries[@]}" -o "$out"/runtime-test
    "$out"/runtime-test
    "$CC" -std=c11 "${cflags[@]}" -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o "$out"/file-mods-test
    "$out"/file-mods-test
    "$CC" -std=c11 "${cflags[@]}" -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" "$out/libatrac9.a" -lm -lz -ldl "${gpu[@]}" "${libraries[@]}" -o "$out"/sema-test
    "$out"/sema-test
    "$CC" -std=c11 -D_GNU_SOURCE "${cflags[@]}" -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o "$out"/content-test
    "$out"/content-test
fi
