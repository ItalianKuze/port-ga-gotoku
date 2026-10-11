# Building PortGaGotoku

## Windows (maintained)

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 UCRT64** shell.
2. Install the toolchain and libraries:

   ```sh
   pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,python,python-packaging} \
       mingw-w64-ucrt-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,shaderc,glslang,spirv-tools,spirv-cross} \
       mingw-w64-ucrt-x86_64-{sdl3,ffmpeg,boost,fmt,robin-map,xxhash,zydis,zlib}
   ```

3. Clone with the submodules and build:

   ```sh
   git clone --recursive <this repository> PortGaGotoku
   cd PortGaGotoku
   bash scripts/build-win.sh
   ```

   `build-win.sh` applies `gpu/patches/fsr-vulkan/*.patch` to the FSR-Vulkan submodule, builds the GPU
   library and the runtime in three passes (they import from each other), and lays out `out-win/run`:
   `bb-probe.exe`, `bbport_runtime.dll`, `libbbgpu.dll` and the UCRT64 DLLs they need (never
   `vulkan-1.dll`: the loader is the driver's). The build id (version in `VERSION`, commit, date) is the
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

5. A package that needs no Python (`dist/PortGaGotoku.zip`, about 70 MB):

   ```sh
   bash packaging/windows/package.sh
   ```

   `PortGaGotoku.exe` (the launcher, frozen with PyInstaller; it also runs the preparation scripts),
   `Start Hokuto.exe` (the game with the saved settings), `bin/` (the game's executables and DLLs),
   `scripts/`, `README-Windows.txt`, `LICENSE`.

Other ways to start: `scripts/Start Hokuto.cmd` (double-click; the data folder and the game folder come
from the launcher's settings, or `PGG_DATA` / `PGG_GAME`), `scripts/run-hokuto-win.sh` (from the
UCRT64 shell; `BB_DATA_DIR`, `BB_GAME_DIR`, `BB_TIMEOUT`).

Tests: `out-win/pyenv-ui/Scripts/python.exe -m unittest tests.test_launcher`.

## Linux (not maintained, to be recovered)

The code stays portable (Windows-only parts are under `#ifdef _WIN32` or in Windows-only files), but the
Linux build and tests have not been run since the work moved to Windows. bbport's instructions still
describe it: `build.sh` (`bash build.sh --test`), `python3 -m unittest discover -s tests`, the GPU tests
under `out/gpu`, and `run-hokuto.sh` to start the game (`BB_GAME_DIR`, `BB_DATA_DIR`). Expect fixes to be
needed.
