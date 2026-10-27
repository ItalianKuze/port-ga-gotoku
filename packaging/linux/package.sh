#!/usr/bin/env bash
# PortGaGotoku: the Linux / Steam Deck AppImage from the release build of build-sniper.sh.
#
#   AppDir (read-only once packed; the game's data go next to the .AppImage, see AppRun)
#     AppRun                start script (asks for the game folder the first time, prepares, plays)
#     portgagotoku.desktop, portgagotoku.svg, .DirIcon
#     bin/                  bb-probe, gpu/libbbgpu.so, lib/ (SDL3, xxhash, Zydis, zlib), build-info.txt
#     scripts/              the boot image preparation (python3 of the system)
#     README.txt, LICENSE, THIRD_PARTY.txt
#
# appimagetool and the AppImage runtime are pinned and checked by SHA-256 (cache: PGG_APPIMAGE_CACHE,
# default ~/.cache/pgg-appimage). A test build is named after the next version and the commit:
# PGG_PACKAGE_VERSION, default 0.1.3-dev-<commit>.
#
# Usage: bash packaging/linux/build-sniper.sh && bash packaging/linux/package.sh
# Output: out-linux/PortGaGotoku-<version>-x86_64.AppImage and PortGaGotoku-<version>-README.txt
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${PGG_SNIPER_CACHE:-$HOME/.cache/pgg-sniper}
tools=${PGG_APPIMAGE_CACHE:-$HOME/.cache/pgg-appimage}
build=$root/out-linux/build
[[ -x $build/bb-probe && -f $build/gpu/libbbgpu.so ]] || { echo 'Build first: bash packaging/linux/build-sniper.sh' >&2; exit 1; }

commit=$(git -C "$root" rev-parse --short HEAD)
git -C "$root" diff --quiet --ignore-submodules HEAD || commit+="-changes"
version=${PGG_PACKAGE_VERSION:-0.1.3-dev-$commit}
name=PortGaGotoku-$version
appdir=$root/out-linux/AppDir
rm -rf "$appdir" && mkdir -p "$appdir/bin/gpu" "$appdir/bin/lib" "$appdir/scripts"

cp "$build/bb-probe" "$appdir/bin/"
cp "$build/gpu/libbbgpu.so" "$appdir/bin/gpu/"
# The libraries the binaries need that SteamOS or another distribution may lack or have in another
# version (by soname, the files the links point to).
cp -L "$cache/deps/lib/libSDL3.so.0" "$cache/deps/lib/libxxhash.so.0" "$cache/deps/lib/libZydis.so.4.1" "$appdir/bin/lib/"
cp -L "$cache/sysroot/lib/x86_64-linux-gnu/libz.so.1" "$appdir/bin/lib/"
printf '%s (%s, %s)\n' "$version" "$commit" "$(date +%Y-%m-%d)" > "$appdir/bin/build-info.txt"
for script in prepare.py link_modules.py link_libc.py content_profile.py game_check.py; do
    cp "$root/scripts/$script" "$appdir/scripts/"
done
install -m 755 "$root/packaging/linux/AppRun" "$appdir/AppRun"
cp "$root/packaging/linux/portgagotoku.desktop" "$root/packaging/linux/portgagotoku.svg" "$appdir/"
ln -s portgagotoku.svg "$appdir/.DirIcon"
cp "$root/packaging/linux/README.txt" "$appdir/README.txt"
cp "$root/LICENSE" "$appdir/LICENSE"
cat > "$appdir/THIRD_PARTY.txt" <<EOF
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
The AppImage runtime (type2-runtime, MIT) mounts this content.
The sources of this version: https://github.com/ItalianKuze/port-ga-gotoku ($commit).
EOF

# ---------------------------------------------------------------- appimagetool and runtime
mkdir -p "$tools"
fetch() {  # file url sha256
    if [[ ! -f $tools/$1 ]] || ! echo "$3  $tools/$1" | sha256sum -c --quiet 2>/dev/null; then
        curl -fsL -o "$tools/$1" "$2"
        echo "$3  $tools/$1" | sha256sum -c --quiet
    fi
}
fetch appimagetool-1.9.1-x86_64.AppImage \
    https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
    ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
fetch runtime-20251108-x86_64 \
    https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
    2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
chmod +x "$tools/appimagetool-1.9.1-x86_64.AppImage"

out=$root/out-linux/$name-x86_64.AppImage
rm -f "$out"
# Extracted rather than mounted: no FUSE needed where the AppImage is built (containers, CI).
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tools/appimagetool-1.9.1-x86_64.AppImage" --no-appstream \
    --runtime-file "$tools/runtime-20251108-x86_64" "$appdir" "$out" > "$root/out-linux/appimagetool.log" 2>&1 ||
    { tail -20 "$root/out-linux/appimagetool.log" >&2; echo 'appimagetool failed' >&2; exit 1; }
cp "$root/packaging/linux/README.txt" "$root/out-linux/$name-README.txt"
echo "$out ($(du -h "$out" | cut -f1))"
