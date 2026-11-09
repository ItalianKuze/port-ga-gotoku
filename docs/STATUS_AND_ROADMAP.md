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

## Done in 0.1.4

- The depth of field in Kenshiro's first close-up is no longer blocky: a compute fill of a stencil
  plane now also clears the image's stencil.
- The bartender minigame can be completed: the motion sensor of a DualShock 4 or DualSense, or the
  right stick (and the arrow keys) up and down for the SHAKE.
- The controller's light bar is blue as soon as it connects.
- Frame pacing on Windows: a high resolution timer for the frame waits, and no power throttling.
- The `PGG_*` variables set by the user reach the game when it is started from the launcher or the
  AppImage.
- Code cleanup: diagnostics and addresses left from bbport's Bloodborne removed.

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

## Development paused

v0.1.4-alpha is the last release. What was planned next, for whoever picks it up: key remapping from
the launcher or the in-game menu (today it is done by hand in `pgg.ini`), a full playthrough noting
the bugs found on the way, tests on the Steam Deck, performance (drops in busy areas, first-run shader
compilation, GPUs with 4 GB) and the flickering of some characters.

## Known issues

- Occasional frame rate drops in busy areas; on weaker PCs, drops and pauses of a few seconds.
- First-run stutter while shaders compile (a cache keeps them for the next runs).
- Some characters flicker (seen on a dog and a woman).
- Movies play at 30 FPS (probably as encoded); occasional audio crackle; about 1 s of delay on some
  tutorial prompts.
- Fullscreen with Alt+Tab has not been tested much.
- No mouse control for the camera; button prompts are PlayStation's.

## Linux

`build.sh` builds the runtime and the GPU library; the release AppImage is built in the Steam Runtime
SDK (`packaging/linux`). Tested so far on a virtual machine and by a tester on a desktop Linux PC; the
Steam Deck was tried by a tester with 0.1.3.
