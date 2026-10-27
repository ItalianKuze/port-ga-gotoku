#!/usr/bin/env bash
# PortGaGotoku: Linux release build inside the Steam Runtime 3 "sniper" SDK (Debian 11, glibc 2.31),
# so the binaries run on SteamOS and on the common distributions.
#
# The SDK's sysroot (Valve's image, verified by SHA-256) is entered with bubblewrap: no root, no
# Docker. It brings gcc 14, cmake, ninja, glslang, SDL3 3.4.18, the Vulkan loader, zlib and X11; the
# other dependencies (and Vulkan headers newer than its 1.4.309) are built from pinned sources into
# the cache (the versions the Ubuntu build uses).
# Then build.sh runs there as a release build (PGG_RELEASE=1): libstdc++/libgcc static, libraries
# found through $ORIGIN, no build paths in the binaries. Result: out-linux/build.
#
# Usage:  bash packaging/linux/build-sniper.sh [--test]
# Env:    PGG_SNIPER_CACHE  SDK and dependencies (default ~/.cache/pgg-sniper; a Linux file system,
#                           the SDK has symbolic links and file modes a Windows drive does not keep)
#         PGG_SNIPER_SDK    SDK sysroot URL (default: latest-public-stable, with its SHA256SUMS)
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${PGG_SNIPER_CACHE:-$HOME/.cache/pgg-sniper}
command -v bwrap >/dev/null || { echo 'Need bubblewrap (bwrap).' >&2; exit 1; }
mkdir -p "$cache/src"

# ---------------------------------------------------------------- the SDK
sdk_base=https://repo.steampowered.com/steamrt-images-sniper/snapshots/latest-public-stable
sdk_name='com.valvesoftware.SteamRuntime.Sdk-amd64,i386-sniper-sysroot.tar.gz'
sdk_url=${PGG_SNIPER_SDK:-$sdk_base/${sdk_name//,/%2C}}
sysroot=$cache/sysroot
if [[ ! -f $sysroot/.complete ]]; then
    echo "== Steam Runtime sniper SDK: $sdk_url =="
    curl -fL -o "$cache/sdk.tar.gz" "$sdk_url"
    expected=$(curl -fsL "$sdk_base/SHA256SUMS" | awk -v n="*$sdk_name" '$2 == n {print $1}')
    [[ -n $expected ]] || { echo 'SDK checksum not found in SHA256SUMS' >&2; exit 1; }
    echo "$expected  $cache/sdk.tar.gz" | sha256sum -c --quiet
    rm -rf "$sysroot" && mkdir -p "$sysroot"
    tar -xzf "$cache/sdk.tar.gz" -C "$sysroot" --no-same-owner --exclude='dev/*'
    curl -fsL "$sdk_base/VERSION.txt" > "$sysroot/.version" || true
    rm "$cache/sdk.tar.gz"
    touch "$sysroot/.complete"
fi
echo "SDK: Steam Runtime sniper $(cat "$sysroot/.version" 2>/dev/null)"

# ---------------------------------------------------------------- dependency sources
# name file url sha256
deps=(
    "boost boost.tar.bz2 https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2 49551aff3b22cbc5c5a9ed3dbc92f0e23ea50a0f7325b0d198b705e8ee3fc305"
    "fmt fmt.tar.gz https://github.com/fmtlib/fmt/archive/refs/tags/10.1.1.tar.gz 78b8c0a72b1c35e4443a7e308df52498252d1cefc2b08c9a97bc9ee6cfe61f8b"
    "magic_enum magic_enum.tar.gz https://github.com/Neargye/magic_enum/archive/refs/tags/v0.9.7.tar.gz b403d3dad4ef542fdc3024fa37d3a6cedb4ad33c72e31b6d9bab89dcaf69edf7"
    "robin-map robin-map.tar.gz https://github.com/Tessil/robin-map/archive/refs/tags/v1.4.1.tar.gz 0e3f53a377fdcdc5f9fed7a4c0d4f99e82bbb64175233bd13427fef9a771f4a1"
    "vma vma.tar.gz https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e"
    "xxhash xxhash.tar.gz https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80"
    "zydis zydis.tar.gz https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz 45c6d4d499a1cc80780f7834747c637509777c01dca1e98c5e7c0bfaccdb1514"
    "vulkan-headers vulkan-headers.tar.gz https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.341.tar.gz 8876aad926b2e72bdefddc34885472d5332e2d20aa3ed57313466f0bc982cf3f"
    "zycore zycore.tar.gz https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz 943f91eb9ab2a8cc01ab9f8b785e769a273502071e0ee8011cdfcaad93947cec"
)
for entry in "${deps[@]}"; do
    read -r name file url sum <<< "$entry"
    if [[ ! -f $cache/src/$file ]] || ! echo "$sum  $cache/src/$file" | sha256sum -c --quiet 2>/dev/null; then
        echo "== download $name =="
        curl -fsL -o "$cache/src/$file" "$url"
        echo "$sum  $cache/src/$file" | sha256sum -c --quiet
    fi
done

# ---------------------------------------------------------------- inside the SDK
# The project at /src, the cache at /cache; the network is not needed in there.
run() {
    bwrap --bind "$sysroot" / --dev /dev --proc /proc --tmpfs /tmp \
        --bind "$root" /src --bind "$cache" /cache --chdir /src \
        --setenv HOME /tmp --setenv PATH /usr/bin:/bin --setenv CC gcc-14 --setenv CXX g++-14 \
        "$@"
}
run bash -euo pipefail -c '
prefix=/cache/deps
stamp=$prefix/.built-$(sha256sum /src/packaging/linux/build-sniper.sh | cut -c1-16)
if [[ ! -f $stamp ]]; then
    echo "== dependencies into $prefix =="
    rm -rf "$prefix" /cache/build && mkdir -p "$prefix/include" /cache/build && cd /cache/build
    for f in /cache/src/*.tar.*; do tar -xf "$f"; done
    # Boost: headers only (the GPU library uses no compiled Boost library).
    mv boost_1_90_0/boost "$prefix/include/"
    cmake_install() {  # source folder, CMake options...
        local source=$1; shift
        cmake -S "$source" -B "$source/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
            -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" >/dev/null
        ninja -C "$source/build" install >/dev/null
    }
    # Vulkan headers newer than the SDK'"'"'s 1.4.309 (the Ubuntu build'"'"'s version; headers only: the
    # player'"'"'s own Vulkan loader runs the game).
    cmake_install Vulkan-Headers-1.4.341
    cmake_install fmt-10.1.1 -DFMT_TEST=OFF -DFMT_DOC=OFF
    cmake_install magic_enum-0.9.7 -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
    cmake_install robin-map-1.4.1
    cmake_install VulkanMemoryAllocator-3.3.0
    make -C xxHash-0.8.3 -j"$(nproc)" PREFIX="$prefix" lib >/dev/null
    make -C xxHash-0.8.3 PREFIX="$prefix" install_libxxhash install_libxxhash.includes install_libxxhash.pc >/dev/null
    # Zydis builds its own copy of Zycore into libZydis.so; Zycore is also installed (static) because
    # the CMake package of Zydis asks for it.
    cp -r zycore-c-1.5.2/. zydis-4.1.1/dependencies/zycore/
    cmake_install zycore-c-1.5.2 -DZYCORE_BUILD_SHARED_LIB=OFF
    cmake_install zydis-4.1.1 -DZYDIS_BUILD_SHARED_LIB=ON -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF \
        -DZYDIS_BUILD_DOXYGEN=OFF
    cd /src && rm -rf /cache/build
    touch "$stamp"
fi
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig:$prefix/share/pkgconfig
export LD_LIBRARY_PATH=$prefix/lib
export PGG_BUILD_DIR=out-linux/build PGG_RELEASE=1
export PGG_GPU_CMAKE_ARGS="-DCMAKE_PREFIX_PATH=$prefix -DBoost_INCLUDE_DIR=$prefix/include -DVulkan_INCLUDE_DIR=$prefix/include"
bash build.sh "$@"
' build-sniper "$@"
echo "== out-linux/build (glibc $(objdump -T "$root/out-linux/build/bb-probe" "$root/out-linux/build/gpu/libbbgpu.so" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)) =="
