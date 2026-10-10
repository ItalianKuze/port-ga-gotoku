#!/usr/bin/env bash
# bbport: launch Hokuto: Lost Paradise (CUSA12781) from the Windows build.
#
# Mirrors run-hokuto.sh: the prepared boot image and content profile come from
# games/bb-win-data/out, the game files from the dump, and the runtime from out-win/run.
#
# Env: BB_DATA_DIR, BB_RUN_DIR, BB_TIMEOUT, BB_ARGS
#   BB_TIMEOUT  seconds before bb-probe's watchdog dumps every thread and stops the game, as
#               run-hokuto.sh passes it (default 30); 0 disables it, for playing.
set -uo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
data=${BB_DATA_DIR:-/c/path/to/PortGaGotoku-data}
game=${BB_GAME_DIR:-/c/path/to/CUSA12781}
run=${BB_RUN_DIR:-$root/out-win/run}
out=$data/out
timeout_s=${BB_TIMEOUT:-30}

[[ -f $out/boot-linked.bin ]] || { echo "No $out/boot-linked.bin (run scripts/prepare.py + link_modules.py)." >&2; exit 1; }
[[ -f $game/eboot.bin ]] || { echo "No eboot.bin in $game." >&2; exit 1; }

mkdir -p "$data/user"
export XDG_DATA_HOME="$data"

cd "$run" || exit 1
echo "== launching (game=$game) =="
# shellcheck disable=SC2086
timeout "$(( timeout_s ? timeout_s + 60 : 0 ))" ./bb-probe.exe "$out/boot-linked.bin" \
    --content-profile "$out/content.bin" \
    --app0 "$game" --user "$data/user" \
    --timeout "$timeout_s" ${BB_ARGS:-} 2>&1
echo "== exit: $? =="