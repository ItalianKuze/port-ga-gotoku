#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# PortGaGotoku: the third-party sources zip of the Windows package (called by package.sh).
#   sources.sh <sources list> <venv python> <zip>
# The list (written by licenses.sh) names the GPL and LGPL components in the package. For each:
#   msys2 <pkgbase> <version>   MSYS2's source package (PKGBUILD, patches, upstream sources) from
#                               repo.msys2.org, checked against its signature with pacman's keyring
#   pypi <project> <version>    the project's sdist from PyPI, checked against PyPI's SHA-256
# Downloads are kept in out-win/sources-cache. Packaging stops if one cannot be fetched or checked.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
list=$1
python=$2
zip=$3
cache=out-win/sources-cache
mkdir -p "$cache"
files=()
while read -r kind name version; do
    case $kind in
    msys2)
        file=$name-$version.src.tar.zst
        if [[ ! -f $cache/$file ]]; then
            curl -sfL -o "$cache/$file.part" "https://repo.msys2.org/mingw/sources/$file"
            curl -sfL -o "$cache/$file.sig" "https://repo.msys2.org/mingw/sources/$file.sig"
            gpgv --keyring /etc/pacman.d/gnupg/pubring.gpg "$cache/$file.sig" "$cache/$file.part" >/dev/null 2>&1 ||
                { echo "sources.sh: bad signature for $file" >&2; rm -f "$cache/$file.part"; exit 1; }
            mv "$cache/$file.part" "$cache/$file"
        fi
        ;;
    pypi)
        read -r url sha < <("$python" - "$name" "$version" <<'EOF' | tr -d '\r'
import json, sys, urllib.request
name, version = sys.argv[1:]
info = json.load(urllib.request.urlopen(f'https://pypi.org/pypi/{name}/{version}/json'))
sdist = next(u for u in info['urls'] if u['packagetype'] == 'sdist')
print(sdist['url'], sdist['digests']['sha256'])
EOF
)
        file=${url##*/}
        if [[ ! -f $cache/$file ]]; then
            curl -sfL -o "$cache/$file.part" "$url"
            [[ $(sha256sum "$cache/$file.part" | cut -d' ' -f1) == "$sha" ]] ||
                { echo "sources.sh: bad SHA-256 for $file" >&2; rm -f "$cache/$file.part"; exit 1; }
            mv "$cache/$file.part" "$cache/$file"
        fi
        ;;
    *) echo "sources.sh: bad line: $kind $name $version" >&2; exit 1 ;;
    esac
    files+=("$file")
done < "$list"

readme=$cache/README.txt
{
    echo 'PortGaGotoku - sources of the GPL and LGPL third-party components'
    echo '================================================================='
    echo
    echo "The source packages of the GPL and LGPL components in $(basename "${zip%-third-party-sources.zip}").zip,"
    echo 'at the exact version shipped there (see THIRD_PARTY_NOTICES.txt in that package).'
    echo
    echo '*.src.tar.zst  MSYS2 source packages (https://packages.msys2.org): PKGBUILD, MSYS2 patches'
    echo '               and the upstream sources, as used to build the DLLs in bin\.'
    echo '*.tar.gz       PyPI source distributions.'
    echo
    echo "PortGaGotoku's own source (GPL-2.0-or-later): https://github.com/ItalianKuze/port-ga-gotoku"
    echo
    for file in "${files[@]}"; do
        printf '%s  %s\n' "$(sha256sum "$cache/$file" | cut -d' ' -f1)" "$file"
    done
} | sed 's/$/\r/' > "$readme"

rm -f "$zip"
# Already compressed: stored as they are.
"$python" - "$(cygpath -w "$zip")" "$(cygpath -w "$cache")" README.txt "${files[@]}" <<'EOF'
import os, sys, zipfile
zip_path, cache, *names = sys.argv[1:]
with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_STORED) as z:
    for name in names:
        z.write(os.path.join(cache, name), name)
EOF
echo "Third-party sources: ${#files[@]} packages, $zip"
