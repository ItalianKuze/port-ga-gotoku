#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")" && pwd)
game=${PGG_GAME_DIR:-${BB_GAME_DIR:-$HOME/games/hokuto_dump/CUSA12781}}
data=${PGG_DATA_DIR:-${BB_DATA_DIR:-/tmp/bbport-hokuto-cusa12781-attempt-02}}
out=$data/out
probe=${PGG_PROBE:-${BB_PROBE:-$root/out/bb-probe}}

if [[ ! -f $game/eboot.bin ]]; then
    echo "No eboot.bin in $game (set PGG_GAME_DIR)." >&2
    exit 1
fi
if [[ ! -x $probe ]]; then
    echo "Probe not found: $probe. Build it with bash build.sh (dependencies: docs/BUILDING.md)." >&2
    exit 1
fi

mkdir -p "$out" "$data/user"
# shadPS4's UserPaths are initialized while libbbgpu is loaded, before
# bbgpu_init can set PGG_GPU_USER_DIR. Keep that static initialization and the
# port settings file inside this attempt's writable data directory.
export XDG_DATA_HOME="$data"
# The GPU library's folders in the data folder, as on Windows (scripts/run-hokuto-win.sh, the
# launcher): its user folder and the shader/pipeline cache (<data>/cache/<serial>).
export PGG_GPU_USER_DIR="$data/gpu" PGG_GPU_CACHE_DIR="$data/cache"
# pgg.ini (bbport.ini until 0.1.2, copied the first time; the old file stays).
[[ -f $data/pgg.ini || ! -f $data/bbport.ini ]] || cp "$data/bbport.ini" "$data/pgg.ini"
export PGG_CONFIG="$data/pgg.ini"
python3 "$root/scripts/prepare.py" "$game" --experimental-elf --out "$out"
# link_modules.py handles both bundled modules and the main ELF. The older
# link_libc.py path requires SELF and rejects this already extracted ELF.
python3 "$root/scripts/link_modules.py" "$game" --out "$out"
python3 "$root/scripts/content_profile.py" "$game" --out "$out" --sku "${PGG_CONTENT_SKU:-${BB_CONTENT_SKU:-full}}"

# Bloodborne XML patches are omitted.
unset PGG_GUEST_IN_PLACE BB_GUEST_IN_PLACE
exec "$probe" "$out/boot-linked.bin" \
    --content-profile "$out/content.bin" \
    --app0 "$game" --user "${PGG_USER_DIR:-${BB_USER_DIR:-$data/user}}" \
    --timeout "${PGG_TIMEOUT:-${BB_TIMEOUT:-30}}" "$@"
