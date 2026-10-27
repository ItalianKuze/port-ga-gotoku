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
# Env:    PGG_BUILD_TYPE (RelWithDebInfo), PGG_LTO (ON)
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${PGG_BUILD_DIR:-${BB_BUILD_DIR:-$root/out-win}}
gpu_dir=$build/gpu
rt_dir=$build/runtime

# Everything below runs against the UCRT64 toolchain; enter the UCRT64 shell if we are not in it.
if [[ ! -x /ucrt64/bin/gcc.exe ]]; then
    echo "Run this from the MSYS2 UCRT64 shell (gcc must be on PATH)." >&2
    exit 1
fi
export PATH=/ucrt64/bin:$PATH
export PKG_CONFIG_PATH=/ucrt64/lib/pkgconfig:/ucrt64/share/pkgconfig

build_type=${PGG_BUILD_TYPE:-${BB_BUILD_TYPE:-RelWithDebInfo}}
lto=${PGG_LTO:-${BB_LTO:-ON}}

# The source tree's location stays out of the binaries: __FILE__ (log messages, asserts) and the
# debug information name files relative to it. CMake hands gcc Windows paths (C:/...).
prefix_map="-ffile-prefix-map=$(cygpath -m "$root")=. -ffile-prefix-map=$root=."
flags=(-DCMAKE_C_FLAGS="$prefix_map" -DCMAKE_CXX_FLAGS="$prefix_map")

if [[ ${1:-} == --clean ]]; then
    echo "== clean =="
    # Everything but the launcher's Python environment (pyenv-ui: pywebview, PyInstaller), which
    # this script does not rebuild.
    if [[ -d $build ]]; then
        find "$build" -mindepth 1 -maxdepth 1 ! -name pyenv-ui -exec rm -rf {} +
    fi
fi

mkdir -p "$build"

# The build's identity, printed first in every log and shown by the launcher: the version in
# VERSION, the commit, "+changes" when the working tree differs from it (submodules aside), and
# the build date.
version=$(tr -d '[:space:]' < "$root/VERSION")
commit=$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)
git -C "$root" diff --quiet --ignore-submodules HEAD 2>/dev/null || commit+="+changes"
build_id="$version ($commit, $(date +%Y-%m-%d))"
echo "== build $build_id =="

# Submodules, as in build.sh.
if [[ ! -f $root/gpu/third_party/imgui/imgui.h ]]; then
    git -C "$root" submodule update --init --recursive
fi

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
    -DCMAKE_BUILD_TYPE="$build_type" -DBB_LTO="$lto" -DBB_PGO=off "${flags[@]}" \
    -DBBPORT_RUNTIME_LIB="$build/libbbport_runtime_stub.dll.a" >/dev/null
ninja -C "$gpu_dir" bbgpu

# ---------------------------------------------------------------- pass 2: the real runtime
echo "== pass 2: bbport_runtime.dll against libbbgpu =="
cmake -S "$root/src" -B "$rt_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" "${flags[@]}" \
    -DBBPORT_GPU_LIB="$gpu_dir/libbbgpu.dll.a" -DBB_BUILD_ID="$build_id" >/dev/null
ninja -C "$rt_dir"

# ---------------------------------------------------------------- pass 3: relink the GPU
# CMake names the import library with the platform "lib" prefix even though the DLL itself is
# named without it, so the .a and the .dll have different stems here.
echo "== pass 3: relink libbbgpu.dll against the real runtime =="
cmake -S "$root/gpu" -B "$gpu_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" -DBB_LTO="$lto" -DBB_PGO=off "${flags[@]}" \
    -DBBPORT_RUNTIME_LIB="$rt_dir/libbbport_runtime.dll.a" >/dev/null
ninja -C "$gpu_dir" bbgpu

# ---------------------------------------------------------------- lay out the run directory
# bb-probe.exe finds bbport_runtime.dll next to itself, and the DLLs find libbbgpu.dll there too.
# The directory is rebuilt from scratch every time, so it holds exactly what this build needs.
run_dir=$build/run
# Empty it rather than remove it: a bb-probe still running (or stuck exiting) keeps the directory
# itself busy on Windows, while its files can still be deleted.
mkdir -p "$run_dir"
find "$run_dir" -mindepth 1 -delete
cp -f "$gpu_dir/libbbgpu.dll" "$run_dir/"
cp -f "$rt_dir/bbport_runtime.dll" "$run_dir/"
cp -f "$rt_dir/bb-probe.exe" "$run_dir/"
printf '%s\n' "$build_id" > "$run_dir/build-info.txt"

# Runtime DLLs from the MSYS2 UCRT64 prefix, found recursively from the import tables (what
# ntldd -R reports, limited to that prefix). Windows' own DLLs (System32, api-ms-win-*, ucrtbase)
# are never copied, and neither is vulkan-1.dll: the Vulkan loader must be the one the GPU driver
# installed in System32. DLLs loaded at run time with LoadLibrary are not in import tables.
ucrt_bin=/ucrt64/bin
sources=$build/run-dll-sources.txt
: > "$sources"
declare -A seen=()
queue=("$run_dir/bb-probe.exe" "$run_dir/bbport_runtime.dll" "$run_dir/libbbgpu.dll")
while (( ${#queue[@]} )); do
    file=${queue[0]}
    queue=("${queue[@]:1}")
    while read -r name; do
        key=${name,,}
        [[ -n ${seen[$key]:-} ]] && continue
        seen[$key]=1
        case $key in vulkan-1.dll|api-ms-win-*|ext-ms-win-*|ucrtbase.dll) continue ;; esac
        [[ -f $run_dir/$name ]] && continue                  # this build's own modules
        src=$(find "$ucrt_bin" -maxdepth 1 -iname "$name" -print -quit)
        [[ -n $src ]] || continue                             # not in UCRT64: a Windows DLL
        cp -f "$src" "$run_dir/"
        printf '%-28s <- %s (imported by %s)\n' "$(basename "$src")" "$src" "$(basename "$file")" >> "$sources"
        queue+=("$run_dir/$(basename "$src")")
    done < <(objdump -p "$file" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
done

echo
echo "Built into $gpu_dir and $rt_dir; runnable tree in $run_dir"
echo "UCRT64 DLLs copied ($sources):"
cat "$sources"
