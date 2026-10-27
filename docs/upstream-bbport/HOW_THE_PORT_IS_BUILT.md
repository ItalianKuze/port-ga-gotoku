# How bbport is built

English translation of the "Как устроен порт" section of bbport's `ROADMAP.md` (as of 0.4, daa7165).
PortGaGotoku keeps this structure for Fist of the North Star: Lost Paradise (CUSA12781), without the
XML patches; its Windows changes are in `../TECHNICAL_NOTES.md`.

- The original eboot (CUSA03173 v01.09) runs directly on x86-64. The PS4 system libraries are replaced
  by an HLE runtime of its own, in C (`runtime_*.c`, `probe.c`).
- Graphics: shadPS4's video core and shader recompiler (GPL), built into `libbbgpu.so` (`gpu/`), with
  changes of its own. The shadPS4 emulator as a whole is not used.
- Game patches (FPS, resolution and others) are the community's XML (`patches/Bloodborne.xml`),
  compiled by `patches.py` and applied by the loader at start.
