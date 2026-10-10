# Fallback module for platforms with no magic_enum CMake config (e.g. MSYS2/UCRT64).
# The config package (Nix, vcpkg, system) is preferred first, so Linux builds keep
# resolving magic_enum exactly as before.
find_package(magic_enum CONFIG QUIET)
if (magic_enum_FOUND)
    return()
endif()

find_path(MAGIC_ENUM_INCLUDE_DIR
    NAMES magic_enum/magic_enum.hpp
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/magic_enum/include)
if (NOT MAGIC_ENUM_INCLUDE_DIR)
    set(magic_enum_FOUND FALSE)
    set(magic_enum_NOT_FOUND_MESSAGE
        "magic_enum headers not found; expected magic_enum/magic_enum.hpp under third_party/magic_enum/include")
    return()
endif()

add_library(magic_enum::magic_enum INTERFACE IMPORTED)
set_target_properties(magic_enum::magic_enum PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MAGIC_ENUM_INCLUDE_DIR}")
set(magic_enum_FOUND TRUE)