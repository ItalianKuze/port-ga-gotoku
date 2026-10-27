# Building PortGaGotoku

## Windows (maintained)

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 UCRT64** shell.
2. Install the toolchain and libraries:

   ```sh
   pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,python,python-packaging} \
       mingw-w64-ucrt-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,shaderc,glslang,spirv-tools,spirv-cross} \
       mingw-w64-ucrt-x86_64-{sdl3,boost,fmt,robin-map,xxhash,zydis,zlib}
   ```

3. Clone with the submodules and build:

   ```sh
   git clone --recursive <this repository> PortGaGotoku
   cd PortGaGotoku
   bash scripts/build-win.sh
   ```

   `build-win.sh` builds the GPU library and the runtime in three passes (they import from each other),
   and lays out `out-win/run`: `bb-probe.exe`, `bbport_runtime.dll`, `libbbgpu.dll` and the UCRT64 DLLs
   they need (never `vulkan-1.dll`: the loader is the driver's). The build id (version in `VERSION`, commit, date) is the
   first line of every game log and is in `out-win/run/build-info.txt`.

4. The launcher, from the sources: a Windows Python 3 from python.org (not MSYS2's) and a private
   environment with pywebview:

   ```sh
   py -m venv out-win/pyenv-ui            # or: <python.org python.exe> -m venv out-win/pyenv-ui
   out-win/pyenv-ui/Scripts/python.exe -m pip install pywebview pyinstaller
   out-win/pyenv-ui/Scripts/pythonw.exe launcher/pgg_main.py
   ```

   On the first start it asks for the game folder (CUSA12781), prepares the boot image into
   `PortGaGotoku-data/out` (`scripts/prepare.py`, `link_modules.py`, `content_profile.py`; the game
   files are only read) and starts the game.

5. A package that needs no Python (`dist/PortGaGotoku-<version>.zip`, about 70 MB, and the
   third-party sources zip next to it):

   ```sh
   bash packaging/windows/package.sh
   ```

   `PortGaGotoku.exe` (the launcher, frozen with PyInstaller; it also runs the preparation scripts),
   `Start Hokuto.exe` (the game with the saved settings), `bin/` (the game's executables and DLLs),
   `scripts/`, `README-Windows.txt`, `LICENSE`.

Other ways to start: `scripts/Start Hokuto.cmd` (double-click; the data folder and the game folder come
from the launcher's settings, or `PGG_DATA` / `PGG_GAME`), `scripts/run-hokuto-win.sh` (from the
UCRT64 shell; `PGG_DATA_DIR`, `PGG_GAME_DIR`, `PGG_TIMEOUT`).

Tests: `out-win/pyenv-ui/Scripts/python.exe -m unittest tests.test_launcher`.

## Linux

`bash build.sh` (tests: `bash build.sh --test`) builds `out/bb-probe` and `out/gpu/libbbgpu.so`. It needs gcc
(C++23: gcc 13 or newer), cmake, ninja, git, pkg-config and the development packages of Vulkan, Boost
(headers), fmt, magic_enum, tsl-robin-map, VulkanMemoryAllocator, xxhash, Zydis, X11, zlib and glslang (on
Ubuntu: `libvulkan-dev libboost-dev libfmt-dev libmagicenum-dev robin-map-dev
libvulkan-memory-allocator-dev libxxhash-dev libzydis-dev libx11-dev zlib1g-dev glslang-tools`). SDL3 comes
from pkg-config; when the system has none (Ubuntu 24.04), or with `PGG_SDL3_SOURCE=1`, `build.sh` builds the
pinned release (the Windows build's version) into `out/deps/sdl3`. Last tried on Ubuntu 26.04 (WSL).
`run-hokuto.sh` starts the game (`PGG_GAME_DIR`, `PGG_DATA_DIR`). The release build for SteamOS (old glibc,
libraries in the package) is still to come.
