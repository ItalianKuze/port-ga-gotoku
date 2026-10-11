# Status and roadmap

## Status (Windows 11, NVIDIA GeForce RTX 5070)

Works: logos, title screen, menus, audio, movies, gameplay (exploration, combat with guard), gamepad
(XInput; light bar calls handled) and keyboard, saving and Continue, closing with the window's X, the
launcher (game folder check, boot image preparation, settings, log, stop), the in-game menu, the
statistics HUD, the automatic benchmark.

Measured (90 s, warm shader cache, Mailbox, window):

| Scene | FPS | 1% low | CPU of the GPU command thread | GPU busy |
|---|---|---|---|---|
| standing outside the city | 60.0 | 55 | ~8 ms per frame | ~3.5 ms |
| walking towards the gate of Eden | 48–54 | 20–25 | 12–17 ms per frame | 5–7 ms |

## Known issues

- **Frame rate while walking**: drops to 30 (and 20) FPS frames in busy areas. The thread
  `shadPS4:GpuCommandProcessor` (translating the game's GPU commands and handing the work to the
  recording threads) reaches a full core: about 7 µs per draw, 1100–3700 draws per frame while walking.
  The GPU is busy about a third of the time; the system CPU is mostly idle. Not the shaders (no compiles
  during the measurement) and not the present mode.
- **Flicker on the soldiers' hoods at the gate of Eden**: to compare with the PS4; possible causes are
  depth precision (z-fighting), the material shader, or the texture / render-target cache. RenderDoc
  capture planned.
- Movies play at 30 FPS (probably as encoded); occasional audio crackle; about 1 s of delay on some
  tutorial prompts. To compare with the PS4.
- Only one machine tested. Fullscreen with Alt+Tab, the launcher's Stop button and the package's first
  start on a new machine have not been tried yet.
- Linux is not maintained in this fork for now.

## Next steps

1. **Analysis of the command stream**: command buffers and draws per frame, where the GPU command
   thread spends its time, how much of it the parallel paths (draw preparation workers, recording
   threads, texture bind helper, asynchronous submission) take on Windows.
2. **Comparison with current shadPS4** for the command processor and the caches.
3. **Optimisations**, each measured with the benchmark (standing and walking, three runs) and kept only
   if it helps.
4. The benchmark is ready: `scripts/bench_hokuto.py walk --runs 3` / `still --runs 3`.

## Planned features

- On-screen save list (load / overwrite with confirmation) on the in-game menu.
- Importing saves exported from a PS4 (Apollo Save Tool): `scripts/import_ps4_save.py` is written but
  untested on real data.
- A graphics profile for Hokuto (output resolution, options that act on this game).
- An upscaler for Hokuto. bbport's temporal upscaler (FSR 3.1 / FSR 4 / TAA, `vk_temporal_upscaler`) stays
  in the renderer as its base: today it is tied to a Bloodborne shader (0x9a9cf8a9) and never runs on
  Hokuto; it needs Hokuto's scene colour, depth and motion vectors found the same way.
- Monitor choice in the launcher; restoring a save from its backups from the launcher.
- Smaller package (the UCRT64 DLLs copied by import tables include more than the game needs).

## Linux recovery

`git pull` on a Linux machine, `bash build.sh --test`, the Python and GPU tests, `run-hokuto.sh`, and the
fixes the Windows-only period made necessary; `run-hokuto.sh` should use the same data layout as the
launcher.
