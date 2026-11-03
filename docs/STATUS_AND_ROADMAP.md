# Status and roadmap

## Status

Developed and tested on Windows 11 with an NVIDIA GeForce RTX 5070; testers have run it on AMD Radeon
RX 9000 cards (Windows and Linux) and on a 4 GB laptop GPU.

Works: logos, title screen, menus, audio, movies, gameplay (exploration, combat), gamepad (XInput,
vibration, light bar) and keyboard, saving and Continue, the on-screen save list, the launcher (game
folder check, boot image preparation, settings, Saves page with import and export, log, stop), the
in-game menu, the statistics HUD, closing with the window's X. The 01.09 data (Special Packs) is read
from a patch folder next to the dump; the packs themselves (from Chogre, chapter 5) have not been
checked in game yet.

Frame rate: about 60 FPS while walking on the development machine, with occasional drops.

## Done in 0.1.3

- The grain around characters on AMD Radeon RX 9000 cards: forced 16x anisotropic filtering is now an
  option of the in-game menu (off by default, the game's own filtering); on AMD GPUs it stays off.
  If the grain comes back, `PGG_GPU_OFF=textures` switches off the whole group.
- Alt+Enter switches between window and fullscreen.
- The 01.09 patch folder (`<dump>-patch` or `<dump>-UPDATE`) is read over the base game, data only.
- Keyboard: Circle is V (it was Left Shift); Windows key shortcuts no longer reach the game.
- On-screen save list, the launcher's Saves page, import and export of saves from and to shadPS4 and
  a PS4 (decrypted).
- Smaller package (about 20 MB), GPL-2.0-or-later as a whole.
- Experimental Linux AppImage.

## Next, in this order

1. **Motion sensor**: the bartender minigame cannot be completed without it. The controller's gyroscope
   where there is one, a simulated input for controllers without.
2. **Key remapping** from the launcher or the in-game menu (today it is done by hand in `pgg.ini`).
3. **A full playthrough**, from the start to the end, noting the bugs found on the way.
4. **Steam Deck**: tests of the Linux build on the Deck and on a desktop Linux GPU.
5. **Beta**, when the points above hold.

Alongside, when they come: performance (drops in busy areas, first-run shader compilation, GPUs with
4 GB), the blocky depth of field in Kenshiro's first close-up (also on shadPS4: a video core issue).

## Known issues

- Occasional frame rate drops in busy areas; on weaker PCs, drops and pauses of a few seconds.
- First-run stutter while shaders compile (a cache keeps them for the next runs).
- Blocky depth of field in the prologue's first close-up of Kenshiro.
- Movies play at 30 FPS (probably as encoded); occasional audio crackle; about 1 s of delay on some
  tutorial prompts.
- Fullscreen with Alt+Tab has not been tested much.
- No mouse control for the camera; button prompts are PlayStation's.

## Linux

`build.sh` builds the runtime and the GPU library; the release AppImage is built in the Steam Runtime
SDK (`packaging/linux`). Tested so far on a virtual machine and by a tester on a desktop Linux PC; the
Steam Deck has not been tried yet.
