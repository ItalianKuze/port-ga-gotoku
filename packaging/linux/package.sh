#!/usr/bin/env bash
# PortGaGotoku: the Linux / Steam Deck package (tar.gz) from the release build of build-sniper.sh.
#
#   PortGaGotoku-<version>-linux-x86_64/
#     PortGaGotoku.sh       start script (asks for the game folder the first time, prepares, plays)
#     README.txt            adding it to Steam, logs
#     bin/                  bb-probe, gpu/libbbgpu.so, lib/ (SDL3, xxhash, Zydis, zlib), build-info.txt
#     scripts/              the boot image preparation (python3 of the system)
#     LICENSE, THIRD_PARTY.txt
#
# Usage: bash packaging/linux/build-sniper.sh && bash packaging/linux/package.sh
# Output: out-linux/PortGaGotoku-<version>-linux-x86_64.tar.gz
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${PGG_SNIPER_CACHE:-$HOME/.cache/pgg-sniper}
build=$root/out-linux/build
[[ -x $build/bb-probe && -f $build/gpu/libbbgpu.so ]] || { echo 'Build first: bash packaging/linux/build-sniper.sh' >&2; exit 1; }

version=$(tr -d '[:space:]' < "$root/VERSION")
commit=$(git -C "$root" rev-parse --short HEAD)
git -C "$root" diff --quiet --ignore-submodules HEAD || commit+="+changes"
name=PortGaGotoku-$version-linux-x86_64
stage=$root/out-linux/$name
rm -rf "$stage" && mkdir -p "$stage/bin/gpu" "$stage/bin/lib" "$stage/scripts"

cp "$build/bb-probe" "$stage/bin/"
cp "$build/gpu/libbbgpu.so" "$stage/bin/gpu/"
# The libraries the binaries need that SteamOS or another distribution may lack or have in another
# version (by soname, the files the links point to).
cp -L "$cache/deps/lib/libSDL3.so.0" "$cache/deps/lib/libxxhash.so.0" "$cache/deps/lib/libZydis.so.4.1" "$stage/bin/lib/"
cp -L "$cache/sysroot/lib/x86_64-linux-gnu/libz.so.1" "$stage/bin/lib/"
printf '%s (%s, %s)\n' "$version" "$commit" "$(date +%Y-%m-%d)" > "$stage/bin/build-info.txt"
for script in prepare.py link_modules.py link_libc.py content_profile.py game_check.py; do
    cp "$root/scripts/$script" "$stage/scripts/"
done
install -m 755 "$root/packaging/linux/PortGaGotoku.sh" "$stage/PortGaGotoku.sh"
cp "$root/packaging/linux/README.txt" "$stage/README.txt"
cp "$root/LICENSE" "$stage/LICENSE"
cat > "$stage/THIRD_PARTY.txt" <<EOF
PortGaGotoku $version for Linux is distributed under the GNU GPL version 2 or later (LICENSE).
Built in the Steam Runtime 3 "sniper" SDK. It contains:
  bin/lib/libSDL3.so.0     SDL 3.4.18 (zlib license)            https://github.com/libsdl-org/SDL
  bin/lib/libxxhash.so.0   xxHash 0.8.3 (BSD 2-Clause)          https://github.com/Cyan4973/xxHash
  bin/lib/libZydis.so.4.1  Zydis 4.1.1 + Zycore 1.5.2 (MIT)     https://github.com/zyantific/zydis
  bin/lib/libz.so.1        zlib (zlib license), from the SDK    https://zlib.net
  bin/gpu/libbbgpu.so      shadPS4's video core (GPL-2.0-or-later) and, compiled in: Boost 1.90
                           (Boost Software License), fmt 10.1.1 (MIT), magic_enum 0.9.7 (MIT),
                           robin-map 1.4.1 (MIT), VulkanMemoryAllocator 3.3.0 (MIT), Dear ImGui
                           (MIT), sirit (BSD 3-Clause), xbyak (BSD 3-Clause), half (MIT),
                           libstdc++/libgcc (GPL-3.0 with the GCC Runtime Library Exception),
                           the fonts DejaVu Sans (Bitstream Vera license) and Figtree (SIL OFL 1.1)
  bin/bb-probe             LibAtrac9 (MIT), libgcc (GCC Runtime Library Exception)
The sources of this version: https://github.com/ItalianKuze/port-ga-gotoku ($commit).
EOF

tar -C "$root/out-linux" -czf "$root/out-linux/$name.tar.gz" "$name"
echo "$root/out-linux/$name.tar.gz ($(du -h "$root/out-linux/$name.tar.gz" | cut -f1))"
