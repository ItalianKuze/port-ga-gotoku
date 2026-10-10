# Fallback module for platforms with no VulkanMemoryAllocator CMake config (e.g. MSYS2/UCRT64
# ships the header only). The config package (Nix, vcpkg, system) is preferred first, so Linux
# builds keep resolving GPUOpen::VulkanMemoryAllocator exactly as before.
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (VulkanMemoryAllocator_FOUND)
    return()
endif()

# find_path only searches <prefix>/include, so look for the vendored subdirectory first and
# then step into it: the sources include <vk_mem_alloc.h> unqualified, while distributions put
# the header either at the include root or one level down.
find_path(VMA_HEADER_ROOT NAMES vma/vk_mem_alloc.h)
if (VMA_HEADER_ROOT AND EXISTS "${VMA_HEADER_ROOT}/vk_mem_alloc.h")
    set(VULKAN_MEMORY_ALLOCATOR_INCLUDE_DIR "${VMA_HEADER_ROOT}")
elseif (VMA_HEADER_ROOT)
    set(VULKAN_MEMORY_ALLOCATOR_INCLUDE_DIR "${VMA_HEADER_ROOT}/vma")
endif()

if (NOT VULKAN_MEMORY_ALLOCATOR_INCLUDE_DIR)
    set(VulkanMemoryAllocator_FOUND FALSE)
    set(VulkanMemoryAllocator_NOT_FOUND_MESSAGE
        "VulkanMemoryAllocator headers not found; expected vk_mem_alloc.h")
    return()
endif()

add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_MEMORY_ALLOCATOR_INCLUDE_DIR}")
set(VulkanMemoryAllocator_FOUND TRUE)