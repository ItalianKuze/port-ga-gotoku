#!/usr/bin/env bash
# bbport: builds and runs tests/test_pad.c on Windows, from the MSYS2 UCRT64 shell. The test includes
# src/runtime_pad.c whole and needs nothing else of the build: no game files, no window, no controller
# (its controllers are SDL's virtual ones).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${PGG_BUILD_DIR:-$root/out-win}/tests
mkdir -p "$out"
export PATH=/ucrt64/bin:$PATH
cd "$root"
# shellcheck disable=SC2046
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 \
    -I. -Isrc -Igpu -Igpu/shim $(pkg-config --cflags sdl3) \
    tests/test_pad.c $(pkg-config --libs sdl3) -lbcrypt -o "$out/pad-test.exe"
"$out/pad-test.exe"
