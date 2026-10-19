#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# PortGaGotoku: builds dist/PortGaGotoku/ (and dist/PortGaGotoku.zip) from the Windows build:
#   PortGaGotoku.exe    the launcher, frozen with PyInstaller (players need no Python); it also
#                       runs the preparation scripts (--script) and the game without its window
#                       (--play)
#   Start Hokuto.exe    starts the game with the saved settings (packaging/windows/play.c)
#   bin/                bb-probe.exe, bbport_runtime.dll, libbbgpu.dll and their DLLs (out-win/run)
#   scripts/            the boot image preparation (prepare.py, link_modules.py, ...)
#   THIRD_PARTY_NOTICES.txt, licenses/   the third-party components and their licenses (licenses.sh)
# No game files and no user data are included; PortGaGotoku-data is created next to the folder.
# After Supermedo's packaging/windows/package.sh (bloodborne_pc v1.6).
#
# Run from the MSYS2 UCRT64 shell after `bash scripts/build-win.sh`. Freezing uses a Windows
# Python 3 from python.org (WINPYTHON overrides) and a private venv in out-win/pyenv-ui where pip
# installs pywebview (the window: HTML/CSS in Windows' WebView2, launcher/ui) and PyInstaller.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
run=out-win/run
[[ -f $run/bb-probe.exe && -f $run/build-info.txt ]] || { echo 'Build first: bash scripts/build-win.sh' >&2; exit 1; }

python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Python/pythoncore-3*-64/python.exe \
                     "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe /c/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
venv=out-win/pyenv-ui
if [[ ! -x $venv/Scripts/python.exe ]]; then
    "$python" -m venv "$venv"
fi
"$venv/Scripts/python.exe" -m pip install -q --disable-pip-version-check pywebview pyinstaller
# The preparation scripts run inside PortGaGotoku.exe (--script): the standard modules they import
# must be frozen with it.
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile ctypes.wintypes; do
    hidden+=(--hidden-import "$module")
done
"$venv/Scripts/python.exe" -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name PortGaGotoku --distpath out-win/pyi-dist --workpath out-win/pyi-work --specpath out-win/pyi-work \
    --paths "$(cygpath -w "$PWD/launcher")" --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    --add-data "$(cygpath -w "$PWD/launcher/ui");ui" \
    "$(cygpath -w "$PWD/launcher/pgg_main.py")"

gcc -O2 -municode -mwindows -o out-win/start-hokuto.exe packaging/windows/play.c

dest=out-win/stage/PortGaGotoku
rm -rf -- out-win/stage
mkdir -p "$dest/bin" "$dest/scripts"
cp -r out-win/pyi-dist/PortGaGotoku/. "$dest/"
strip -o "$dest/Start Hokuto.exe" out-win/start-hokuto.exe
# The game's executables without debug information (out-win/run keeps the symbols for crash reports).
for file in "$run"/*; do
    case $file in
        *.exe|*.dll) strip --strip-debug -o "$dest/bin/$(basename "$file")" "$file" ;;
        *) cp "$file" "$dest/bin/" ;;
    esac
done
cp scripts/prepare.py scripts/link_modules.py scripts/link_libc.py scripts/content_profile.py \
   scripts/game_check.py "$dest/scripts/"
cp LICENSE "$dest/"
cp packaging/windows/README-Windows.txt "$dest/"
find "$dest" -name __pycache__ -prune -exec rm -r {} +
bash packaging/windows/licenses.sh "$dest" "$venv/Scripts/python.exe"

mkdir -p dist
rm -f dist/PortGaGotoku.zip
# Python's zipfile rather than Windows PowerShell's Compress-Archive, which writes the entries'
# paths with backslashes (other unzip tools then make files named "PortGaGotoku\bin\...").
(cd out-win/stage && "../../$venv/Scripts/python.exe" - <<'EOF'
import os, zipfile
with zipfile.ZipFile('../../dist/PortGaGotoku.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk('PortGaGotoku'):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            z.write(path, path.replace(os.sep, '/'))
EOF
)
du -sh "$dest" dist/PortGaGotoku.zip
