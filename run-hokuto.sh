#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")" && pwd)
game=${BB_GAME_DIR:-$HOME/games/hokuto_dump/CUSA12781}
data=${BB_DATA_DIR:-/tmp/bbport-hokuto-cusa12781-attempt-02}
out=$data/out
probe=${BB_PROBE:-$root/out/bb-probe}

if [[ ! -f $game/eboot.bin ]]; then
    echo "No eboot.bin in $game (set BB_GAME_DIR)." >&2
    exit 1
fi
if [[ ! -x $probe ]]; then
    echo "Probe not found: $probe. Build it with bash build.sh after satisfying shell.nix dependencies." >&2
    exit 1
fi

mkdir -p "$out" "$data/user"
# shadPS4's UserPaths are initialized while libbbgpu is loaded, before
# bbgpu_init can set BB_GPU_USER_DIR. Keep that static initialization and the
# port settings file inside this attempt's writable data directory.
export XDG_DATA_HOME="$data"
export BB_CONFIG="$data/bbport.ini"
python3 "$root/scripts/prepare.py" "$game" --experimental-elf --out "$out"
# link_modules.py handles both bundled modules and the main ELF. The older
# link_libc.py path requires SELF and rejects this already extracted ELF.
python3 "$root/scripts/link_modules.py" "$game" --out "$out"
python3 "$root/scripts/content_profile.py" "$game" --out "$out" --sku "${BB_CONTENT_SKU:-full}"

# Bloodborne XML patches and opt-in GNM census/observation hooks are omitted.
unset BB_GNM_CENSUS BB_GNM_OBSERVE BB_GNM_TRACE BB_GNM_CENSUS_RANGE BB_GNM_CENSUS_FILE
unset BB_GUEST_IN_PLACE
exec "$probe" "$out/boot-linked.bin" \
    --content-profile "$out/content.bin" \
    --app0 "$game" --user "${BB_USER_DIR:-$data/user}" \
    --timeout "${BB_TIMEOUT:-30}" "$@"
