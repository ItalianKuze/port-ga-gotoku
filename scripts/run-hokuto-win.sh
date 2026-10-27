#!/usr/bin/env bash
# bbport: launch Hokuto: Lost Paradise (CUSA12781) from the Windows build.
#
# Mirrors run-hokuto.sh: the prepared boot image and content profile come from <data>/out (the
# launcher prepares it), the game files from the dump, and the runtime from out-win/run.
#
# Env: PGG_DATA_DIR (default: PortGaGotoku-data next to the project folder, as the launcher),
#      PGG_GAME_DIR (default: game_dir in <data>/launcher.json, written by the launcher),
#      PGG_RUN_DIR, PGG_TIMEOUT, PGG_ARGS
#   PGG_TIMEOUT  seconds before bb-probe's watchdog dumps every thread and stops the game, as
#               run-hokuto.sh passes it (default 30); 0 disables it, for playing.
set -uo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
data=${PGG_DATA_DIR:-${BB_DATA_DIR:-$(cd -- "$root/.." && pwd)/PortGaGotoku-data}}
game=${PGG_GAME_DIR:-${BB_GAME_DIR:-}}
if [[ -z $game && -f $data/launcher.json ]]; then
    # "game_dir": "C:\\path\\to\\CUSA12781" -> /c/path/to/CUSA12781
    game=$(sed -n 's/.*"game_dir": *"\(.*\)".*/\1/p' "$data/launcher.json" | sed 's/\\\\/\\/g')
    [[ -n $game ]] && game=$(cygpath -u "$game")
fi
[[ -n $game ]] || { echo "No game folder: choose it in the launcher or set PGG_GAME_DIR." >&2; exit 1; }
run=${PGG_RUN_DIR:-${BB_RUN_DIR:-$root/out-win/run}}
out=$data/out
timeout_s=${PGG_TIMEOUT:-${BB_TIMEOUT:-30}}

[[ -f $out/boot-linked.bin ]] || { echo "No $out/boot-linked.bin (run scripts/prepare.py + link_modules.py)." >&2; exit 1; }
[[ -f $game/eboot.bin ]] || { echo "No eboot.bin in $game." >&2; exit 1; }

mkdir -p "$data/user"
export XDG_DATA_HOME="$data"
# The GPU library's folders in the data folder, not %APPDATA%\shadPS4 (the real shadPS4's):
# its user folder and the shader/pipeline cache (<data>/cache/<serial>).
export PGG_GPU_USER_DIR="$data/gpu" PGG_GPU_CACHE_DIR="$data/cache"

cd "$run" || exit 1
echo "== launching (game=$game) =="
# shellcheck disable=SC2086
./bb-probe.exe "$out/boot-linked.bin" \
    --content-profile "$out/content.bin" \
    --app0 "$game" --user "$data/user" \
    --timeout "$timeout_s" ${PGG_ARGS:-${BB_ARGS:-}} 2>&1 &
probe=$!
# Last-resort net if the watchdog itself hangs: MSYS `timeout` does not stop a native process, so
# kill it (and any child process) by its Windows PID once PGG_TIMEOUT+60 s have passed.
if (( timeout_s )); then
    winpid=$(cat "/proc/$probe/winpid" 2>/dev/null)
    for ((waited = 0; waited < timeout_s + 60; waited++)); do
        kill -0 "$probe" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$probe" 2>/dev/null && [[ -n $winpid ]]; then
        echo "== external timeout after $((timeout_s + 60)) s: taskkill //PID $winpid //T //F =="
        taskkill //PID "$winpid" //T //F
        sleep 1
        tasklist //FI "PID eq $winpid" | grep -q bb-probe && echo "== bb-probe $winpid STILL RUNNING ==" \
                                                      || echo "== bb-probe $winpid gone =="
    fi
fi
wait "$probe"
echo "== exit: $? =="