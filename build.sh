#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
CC=${CC:-$(command -v cc || command -v gcc || true)}
if [[ -z $CC ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
for tool in pkg-config cmake ninja git; do
    command -v "$tool" >/dev/null || { echo "Need $tool (see docs/BUILDING.md)." >&2; exit 1; }
done
pkg-config --exists vulkan || { echo 'Need the Vulkan loader and headers (pkg-config vulkan).' >&2; exit 1; }
# SDL3 from pkg-config; when the system has none (Ubuntu 24.04), or with PGG_SDL3_SOURCE=1, the
# release below is built from source into out/deps/sdl3 (the same version as the Windows build).
SDL3_RELEASE=release-3.4.18
sdl3=$PWD/out/deps/sdl3
if [[ ${PGG_SDL3_SOURCE:-} == 1 ]] || ! pkg-config --exists sdl3; then
    if [[ ! -f $sdl3/.$SDL3_RELEASE ]]; then
        echo "SDL3: building $SDL3_RELEASE from source into $sdl3"
        rm -rf out/deps/SDL "$sdl3"
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$SDL3_RELEASE" https://github.com/libsdl-org/SDL.git out/deps/SDL
        cmake -S out/deps/SDL -B out/deps/SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$sdl3" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF \
            -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >/dev/null
        ninja -C out/deps/SDL/build install >/dev/null
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
# The GPU library finds the same SDL3 as bb-probe (out/deps/sdl3 when built from source).
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${PGG_LTO:-${BB_LTO:-ON}}" -DBB_PGO_DIR="$PWD/pgo" \
    -DCMAKE_BUILD_RPATH="$(pkg-config --variable=libdir sdl3)" >/dev/null
echo "GPU library: PGO $pgo, LTO ${PGG_LTO:-${BB_LTO:-ON}}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
# An SDL3 built from source is found next to the binary when packaged ($ORIGIN/lib), else in out/deps.
libraries+=(-Wl,-rpath,'$ORIGIN/lib' -Wl,-rpath,"$(pkg-config --variable=libdir sdl3)")
# Runtime sources for this (Linux) build: runtime_win.c and any win32_*.c are the Windows backend.
runtime=()
for source in src/runtime*.c; do
    case $(basename "$source") in runtime_win.c|win32_*.c) ;; *) runtime+=("$source") ;; esac
done
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc src/probe.c src/import_trace.c "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm -lz "${gpu[@]}" "${libraries[@]}" -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# The launcher's gamepad list (--gamepads) and control capture (--read-input).
"$CC" -std=c11 -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm -lz "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm -lz "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
fi
