#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# PortGaGotoku: the third-party notices of the Windows package (called by package.sh).
#   licenses.sh <package folder> <python of the launcher's venv>
# Writes <package folder>/THIRD_PARTY_NOTICES.txt and <package folder>/licenses/:
#   msys2/<package>/    the UCRT64 packages the DLLs in bin/ come from (pacman), with the license
#                       files they install; packages that install none use the texts taken from their
#                       sources in packaging/windows/licenses/msys2/<package>/
#   python/<project>/   CPython, PyInstaller (its bootloader is in PortGaGotoku.exe), pywebview and
#                       its requirements, frozen into the launcher (packaging/windows/licenses/python/
#                       for a project whose wheel has no license file)
#   source/<name>/      the components compiled into bbport_runtime.dll and libbbgpu.dll
# Packaging stops if a DLL's package or a package's license text cannot be found.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
dest=$1
python=$2
[[ -d $dest/bin && -x $python ]] || { echo "usage: licenses.sh <package folder> <venv python>" >&2; exit 1; }
lic=$dest/licenses
notices=$dest/THIRD_PARTY_NOTICES.txt
rm -rf -- "$lic"
mkdir -p "$lic/msys2" "$lic/python" "$lic/source"
export LANG=C LC_ALL=C
rows=$(mktemp)
trap 'rm -f "$rows"' EXIT

# ------------------------------------------------------------------ MSYS2 UCRT64 DLLs
declare -A packages=()
for file in "$dest"/bin/*.dll; do
    name=$(basename "$file")
    case $name in bbport_runtime.dll|libbbgpu.dll) continue ;; esac
    package=$(pacman -Qqo "/ucrt64/bin/$name" 2>/dev/null) || { echo "licenses.sh: no package owns $name" >&2; exit 1; }
    packages[$package]+=" $name"
done
for package in $(printf '%s\n' "${!packages[@]}" | sort); do
    short=${package#mingw-w64-ucrt-x86_64-}
    info=$(pacman -Qi "$package")
    version=$(sed -n 's/^Version *: //p' <<< "$info")
    license=$(sed -n 's/^Licenses *: //p' <<< "$info" | sed 's/spdx://g; s/  */ /g')
    url=$(sed -n 's/^URL *: //p' <<< "$info")
    base=$(sed -n '/^%BASE%$/{n;p;q}' "/var/lib/pacman/local/$package-$version/desc")
    mkdir -p "$lic/msys2/$short"
    copied=0
    while read -r path; do
        [[ -f $path ]] || continue
        rel=${path#/ucrt64/share/licenses/*/}
        mkdir -p "$lic/msys2/$short/$(dirname "$rel")"
        cp "$path" "$lic/msys2/$short/$rel"
        copied=1
    done < <(pacman -Qlq "$package" | grep '^/ucrt64/share/licenses/')
    if (( ! copied )); then
        [[ -d packaging/windows/licenses/msys2/$short ]] || { echo "licenses.sh: no license text for $package" >&2; exit 1; }
        cp -r "packaging/windows/licenses/msys2/$short/." "$lic/msys2/$short/"
    fi
    files=$(for name in ${packages[$package]}; do printf 'bin\\%s ' "$name"; done)
    printf '%s|%s|%s|%s|https://repo.msys2.org/mingw/sources/%s-%s.src.tar.zst (MSYS2 %s)|%s\n' \
        "$short" "$version" "$license" "$url" "$base" "$version" "$base" "${files% }" >> "$rows"
done

# ------------------------------------------------------------------ the launcher's Python
"$python" - "$lic/python" packaging/windows/licenses/python <<'EOF' | tr -d '\r' >> "$rows"
import importlib.metadata as md, pathlib, shutil, sys
from packaging.requirements import Requirement
out, fallback = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])

def license_of(dist):
    meta = dist.metadata
    return meta.get('License-Expression') or next(
        (c.split(' :: ')[-1] for c in meta.get_all('Classifier') or [] if c.startswith('License ::')),
        None) or ((meta.get('License') or '').strip().splitlines() or ['see license files'])[0]

def home_of(dist):
    for entry in dist.metadata.get_all('Project-URL') or []:
        label, _, url = entry.partition(', ')
        if label.lower() in ('source', 'source code', 'repository', 'homepage', 'github'):
            return url
    return dist.metadata.get('Home-page') or f'https://pypi.org/project/{dist.metadata["Name"]}/'

# pywebview and everything it requires on this platform, plus PyInstaller (bootloader).
seen, queue = {}, ['pywebview', 'pyinstaller']
while queue:
    dist = md.distribution(queue.pop())
    key = dist.metadata['Name'].lower()
    if key in seen:
        continue
    seen[key] = dist
    if key == 'pyinstaller':
        continue
    for spec in dist.requires or []:
        req = Requirement(spec)
        if req.marker is None or req.marker.evaluate({'extra': ''}):
            queue.append(req.name)

for key, dist in sorted(seen.items()):
    name = dist.metadata['Name']
    files = [f for f in dist.files or [] if '.dist-info/licenses/' in str(f)
             or f.name.upper().startswith(('LICENSE', 'COPYING'))]
    if not files and (fallback / name).is_dir():
        shutil.copytree(fallback / name, out / name)
    elif not files:
        sys.exit(f'licenses.sh: no license file for {name}')
    for f in files:
        rel = str(f).split('/licenses/', 1)[-1] if '/licenses/' in str(f) else f.name
        target = out / name / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f.locate(), target)
    what = 'PortGaGotoku.exe (bootloader)' if key == 'pyinstaller' else 'PortGaGotoku.exe, _internal\\'
    print(f'{name}|{dist.version}|{license_of(dist)}|{home_of(dist)}|https://pypi.org/project/{name}/{dist.version}/#files|{what}')

base = pathlib.Path(sys.base_prefix)
(out / 'CPython').mkdir(parents=True, exist_ok=True)
shutil.copyfile(base / 'LICENSE.txt', out / 'CPython' / 'LICENSE.txt')
v = sys.version_info
print(f'CPython|{v.major}.{v.minor}.{v.micro}|PSF-2.0 (bundled components: see LICENSE.txt)|https://www.python.org/'
      f'|https://www.python.org/downloads/source/|_internal\\ (python{v.major}{v.minor}.dll, standard library)')
EOF
# pywebview ships Microsoft's WebView2 SDK DLLs, which have no license file in the wheel.
webview2=$(printf '%s\n' "$dest"/_internal/webview/lib/Microsoft.Web.WebView2.*.dll | head -1)
if [[ -f $webview2 ]]; then
    printf '%s|%s|%s|%s|%s|%s\n' 'Microsoft Edge WebView2 SDK' 'as shipped by pywebview' \
        'Microsoft WebView2 SDK license (BSD-style)' 'https://www.nuget.org/packages/Microsoft.Web.WebView2' \
        'binary only; license: https://www.nuget.org/packages/Microsoft.Web.WebView2/License' \
        '_internal\webview\lib\Microsoft.Web.WebView2.*.dll' >> "$rows"
fi

# ------------------------------------------------------------------ compiled from this repository
source_component() {  # name|version|license|home|license file(s)
    local name=$1 version=$2 license=$3 home=$4; shift 4
    mkdir -p "$lic/source/$name"
    for file in "$@"; do cp "$file" "$lic/source/$name/"; done
    printf '%s|%s|%s|%s|%s|%s\n' "$name" "$version" "$license" "$home" \
        'this repository (github.com/ItalianKuze/port-ga-gotoku)' 'compiled into bin\libbbgpu.dll or bin\bbport_runtime.dll' >> "$rows"
}
submodule_rev() { git -C "$1" describe --tags --always 2>/dev/null || git -C "$1" rev-parse --short HEAD; }
source_component 'Dear ImGui' "$(submodule_rev gpu/third_party/imgui)" MIT https://github.com/ocornut/imgui gpu/third_party/imgui/LICENSE.txt
source_component 'FSR-Vulkan' "$(git -C gpu/third_party/fsr-vulkan rev-parse --short HEAD)" MIT \
    https://github.com/deadinside28/bloodborne_pc gpu/third_party/fsr-vulkan/LICENSE.txt
source_component 'AMD FidelityFX SDK' 1.1.4 MIT https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK \
    gpu/third_party/fsr-vulkan/upstream/ffx-1.1.4/sdk/LICENSE.txt
source_component LibAtrac9 "$(submodule_rev third_party/LibAtrac9)" MIT https://github.com/Thealexbarney/LibAtrac9 third_party/LibAtrac9/LICENSE
source_component sirit in-tree MIT https://github.com/shadps4-emu/sirit gpu/third_party/sirit/LICENSE.txt
source_component half in-tree MIT https://half.sourceforge.net/ gpu/third_party/half/LICENSE.txt
source_component magic_enum in-tree MIT https://github.com/Neargye/magic_enum gpu/third_party/magic_enum/LICENSE
source_component xbyak in-tree BSD-3-Clause https://github.com/herumi/xbyak gpu/third_party/xbyak/xbyak/COPYRIGHT
mkdir -p "$lic/source/AMD GCN headers"
sed -n '1,/\*\//p' gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h > "$lic/source/AMD GCN headers/NOTICE.txt"
printf '%s|%s|%s|%s|%s|%s\n' 'AMD GCN headers' in-tree MIT https://github.com/GPUOpen-Drivers \
    'this repository (github.com/ItalianKuze/port-ga-gotoku)' 'bin\libbbgpu.dll' >> "$rows"
source_component 'Figtree font' in-tree OFL-1.1 https://github.com/erikdkennedy/figtree \
    gpu/third_party/fonts/LICENSE-Figtree.txt launcher/ui/fonts/Figtree-OFL.txt
source_component 'DejaVu Sans font' in-tree 'Bitstream-Vera' https://dejavu-fonts.github.io/ gpu/third_party/fonts/LICENSE-DejaVu.txt

# ------------------------------------------------------------------ THIRD_PARTY_NOTICES.txt
{
    echo 'PortGaGotoku - third-party components in this package'
    echo '======================================================'
    echo
    echo 'PortGaGotoku itself (bin\bb-probe.exe, bin\bbport_runtime.dll, bin\libbbgpu.dll, the launcher,'
    echo 'scripts\) is GPL-2.0-or-later (LICENSE), as bbport and shadPS4 whose code it contains:'
    echo 'https://github.com/ItalianKuze/port-ga-gotoku'
    echo
    echo 'Some components below are GPL-3.0-or-later or Apache-2.0 (FFmpeg is built with --enable-gpl'
    echo '--enable-version3), so the combination is distributed under the terms of GPL-3.0, which'
    echo 'PortGaGotoku'"'"'s GPL-2.0-or-later permits. The license texts are in licenses\. The source of'
    echo 'every MSYS2 package is at its "Source" link (the exact version shipped here).'
    echo
    while IFS='|' read -r name version license home source files; do
        echo "$name $version"
        echo "    License:  $license"
        echo "    Project:  $home"
        echo "    Source:   $source"
        echo "    Files:    $files"
        echo
    done < "$rows"
} | sed 's/$/\r/' > "$notices"
echo "Third-party notices: $(grep -c '^    License:' "$notices") components, $notices"
