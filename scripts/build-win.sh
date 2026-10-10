#!/usr/bin/env bash
# bbport: Windows build of the GPU library and the HLE runtime.
#
# The two modules import from each other -- libbbgpu.dll imports runtime_* from the runtime,
# and the runtime calls bbgpu_* -- which a PE loader accepts but a build graph cannot express
# directly. This bootstraps in three passes:
#
#   1. runtime stub  -> link libbbgpu.dll against it (produces libbbgpu.dll.a)
#   2. runtime       -> link bbport_runtime.dll against libbbgpu.dll.a  (real implementations)
#   3. GPU           -> relink libbbgpu.dll against bbport_runtime.dll.a, so the import table
#                       names the real runtime module instead of the stub
#
# Usage:  bash scripts/build-win.sh [--clean]
# Env:    BB_BUILD_TYPE (RelWithDebInfo), BB_LTO (ON)
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${BB_BUILD_DIR:-$root/out-win}
gpu_dir=$build/gpu
rt_dir=$build/runtime

# Everything below runs against the UCRT64 toolchain; enter the UCRT64 shell if we are not in it.
if [[ ! -x /ucrt64/bin/gcc.exe ]]; then
    echo "Run this from the MSYS2 UCRT64 shell (gcc must be on PATH)." >&2
    exit 1
fi
export PATH=/ucrt64/bin:$PATH
export PKG_CONFIG_PATH=/ucrt64/lib/pkgconfig:/ucrt64/share/pkgconfig

build_type=${BB_BUILD_TYPE:-RelWithDebInfo}
lto=${BB_LTO:-ON}

if [[ ${1:-} == --clean ]]; then
    echo "== clean =="
    rm -rf "$build"
fi

mkdir -p "$build"

# ---------------------------------------------------------------- pass 1: GPU against a stub
# The stub only has to satisfy the linker; it is replaced in pass 3 and never loaded.
stub_src=$build/runtime_stub.c
stub_def=$root/gpu/cmake/bbport_runtime_imports.txt
{
    echo "/* generated: link-time stand-in for the HLE runtime API, replaced in pass 3 */"
    grep -E '^runtime_' "$stub_def" | sed 's/^/char /; s/$/[1];/'
} > "$stub_src"
gcc -shared -o "$build/libbbport_runtime_stub.dll" \
    -Wl,--out-implib,"$build/libbbport_runtime_stub.dll.a" "$stub_src"

echo "== pass 1: libbbgpu.dll against the runtime stub =="
cmake -S "$root/gpu" -B "$gpu_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" -DBB_LTO="$lto" -DBB_PGO=off \
    -DBBPORT_RUNTIME_LIB="$build/libbbport_runtime_stub.dll.a" >/dev/null
ninja -C "$gpu_dir" bbgpu

# ---------------------------------------------------------------- pass 2: the real runtime
echo "== pass 2: bbport_runtime.dll against libbbgpu =="
cmake -S "$root/src" -B "$rt_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DBBPORT_GPU_LIB="$gpu_dir/libbbgpu.dll.a" >/dev/null
ninja -C "$rt_dir"

# ---------------------------------------------------------------- pass 3: relink the GPU
# CMake names the import library with the platform "lib" prefix even though the DLL itself is
# named without it, so the .a and the .dll have different stems here.
echo "== pass 3: relink libbbgpu.dll against the real runtime =="
cmake -S "$root/gpu" -B "$gpu_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" -DBB_LTO="$lto" -DBB_PGO=off \
    -DBBPORT_RUNTIME_LIB="$rt_dir/libbbport_runtime.dll.a" >/dev/null
ninja -C "$gpu_dir" bbgpu

# ---------------------------------------------------------------- lay out the run directory
# bb-probe.exe finds bbport_runtime.dll next to itself, and the DLLs find libbbgpu.dll there too.
run_dir=$build/run
mkdir -p "$run_dir"
cp -f "$gpu_dir/libbbgpu.dll" "$run_dir/"
cp -f "$rt_dir/bbport_runtime.dll" "$run_dir/"
cp -f "$rt_dir/bb-probe.exe" "$run_dir/"
# Third-party runtime dependencies that are not on the system path.
for dll in /ucrt64/bin/{SDL3.dll,libxxhash.dll,libZydis.dll,vulkan-1.dll,libstdc++-6.dll,libgcc_s_seh-1.dll,libwinpthread-1.dll} \
           /ucrt64/bin/{avcodec-63.dll,avformat-63.dll,avutil-61.dll,swresample-7.dll,swscale-10.dll}; do
    [[ -f $dll ]] && cp -f "$dll" "$run_dir/"
done

echo
echo "Built into $gpu_dir and $rt_dir; runnable tree in $run_dir"
ls -1 "$run_dir"