# Technical notes

What PortGaGotoku changed on top of bbport 0.4 (daa7165) to run Fist of the North Star: Lost Paradise
on Windows, and why. Useful to whoever continues the work, and to upstream where a change is general.

## Windows runtime

- **Guest memory**: the PS4 user range is reserved first as a placeholder and carved up with
  `VirtualAlloc2` / `MapViewOfFile3` (16 KiB granularity), so the guest image lands at its address and
  Vulkan drivers allocate elsewhere (`ERROR_MAPPED_ALIGNMENT` otherwise). `bb-probe.exe` is linked
  without high-entropy ASLR: its stack used to land inside the guest range.
- **TLS**: the guest's thread control block lives in a TEB TLS slot, as the PS4 code expects it at
  `fs:0`.
- **zlib**: the libSceZlib HLE was compiled out on Windows; built again (`<unistd.h>` before zlib, or
  its `FAR` macro clashes).
- **Files**: game files are opened in binary mode (`O_BINARY`; text mode broke reads). The guest's
  rename replaces an existing destination (`MoveFileExA` with `MOVEFILE_REPLACE_EXISTING`): the CRT's
  rename fails there, and the game saves by renaming a temporary file over the old one.
- **Mutex protocol**: winpthreads has no `PTHREAD_PRIO_INHERIT`; `scePthreadMutexInit` returned
  ENOTSUP and the game's CRI middleware skipped its initialisation (stuck before the logo). The protocol
  is now advisory on Windows, as in shadPS4; ENOTSUP returned to the guest is logged once per import.
- **Audio pacing**: `clock_nanosleep(CLOCK_MONOTONIC)` does not exist in winpthreads; the audio output
  thread spun (no sound, frozen logo). It waits on a high-resolution waitable timer.
- **Faults**: one vectored exception handler; faults inside `IsBadReadPtr` are left to Windows
  (Mrsuss60 69b954f); a crash prints its report and ends with `TerminateProcess` (exit code 128 + the
  exception), as does a STOP on an unimplemented import (exit code 20) — `ExitProcess` hung in DLL
  detach.
- **Speculative reads across DLLs**: the GPU's draw preparation reads guest memory ahead of the game
  and recovers from a fault. The recovery point was a thread-local variable — but `__thread` is not
  shared between `bbport_runtime.dll` and `libbbgpu.dll`, so libbbgpu's recovery points never reached
  the handler and a warm shader cache crashed the game in `PipelineCache::GetProgram`. Now one type,
  `BbRecoverBuf` (`gpu/shim/bbport_recover.h`, the Mrsuss60 register layout), is set with
  `runtime_setjmp` and armed through `runtime_recover_arm`, both in the runtime.
- **Closing the window**: `std::_Exit` went through `ExitProcess` and hung. The window's X now calls
  `runtime_quit`: it waits up to 5 s while a `/savedataN` file is open or a slot is mounted for writing,
  prints the report and ends with `TerminateProcess`, exit code 0.
- **Environment variables**: `PGG_<name>`, read through `pgg_getenv` (`gpu/shim/pgg_env.h`, runtime and
  GPU library) and `env_var` (launcher); bbport's `BB_<name>` is still read when `PGG_<name>` is unset.
- **Watchdog and diagnostics**: `PGG_TIMEOUT` dumps every thread without taking locks; `PGG_FILE_TRACE`,
  `PGG_SAVE_TRACE`, `PGG_IMPORT_TRACE`, `PGG_IMPORT_REPORT`, `PGG_PAD_TRACE`, `PGG_EXIT_TRACE`.

## PS4 libraries (as shadPS4 implements them)

- **MsgDialog**: `GetResult`, `GetStatus`, `Close`, `ProgressBar*` (the main menu stopped on
  `sceMsgDialogGetResult`).
- **SaveDataDialog**: the LIST mode returned no slot, so every save created a new slot and Continue
  loaded nothing. It now answers like shadPS4: the slot `focusPos` points at (latest, first, a given
  name) or a new one; user and system messages answer YES/OK.
- **Save backups**: a slot is copied to `save-backups/<slot>-<date-time>/` before the game mounts it for
  writing (the last 10 per slot are kept).
- **Imports**: libkernel (`scePthreadGetname`, `sceKernelMlock`, `IsAddressSanitizerEnabled`,
  `_nanosleep`, and the game's libc imports of functions already implemented), libSceSysmodule,
  libScePosix (`sched_yield`), libScePlayGo, libSceNpTrophy, libSceErrorDialog, libSceScreenShot,
  `scePadSetLightBar` / `ResetLightBar` (combat stopped on it). Unimplemented imports stay a visible
  STOP: no generic stubs.

## GPU library

- **Folders**: the shadPS4 code kept its user folder and shader cache in `%APPDATA%\shadPS4`, which a
  real shadPS4 installation owns. `PGG_GPU_USER_DIR` / `PGG_GPU_CACHE_DIR` are always honoured and the
  launcher puts both in `PortGaGotoku-data`; `pgg.ini` too (`PGG_CONFIG`).
- **Vulkan device**: every device is listed with the reason for the choice; `PGG_GPU_DEVICE` (part of the
  name) or `PGG_GPU_ID` forces one (a laptop with an AMD iGPU and an NVIDIA dGPU loads both ICDs).
- **Game profile**: `gpu/shim/bbport_game.*` maps the TITLE_ID to a game (Hokuto only). The in-game
  menu shows the language (English / Italian), the FPS counter, the statistics HUD and the
  measurement. bbport's Bloodborne-specific parts were removed from this fork: `patches/Bloodborne.xml`
  with `scripts/patches.py` and `scripts/mods.py`, the hooks in Bloodborne's code
  (`bbport_guest_hooks`), the menu's Bloodborne entries and patch effects, the save "sound hack", the
  Linux launcher and AppImage packaging, the FSR 4 asset tools. `scripts/game_check.py` accepts only
  CUSA12781 (another version than 01.00 is a warning).
- **Statistics HUD and measurement** (`gpu/shim/bbport_perf.*`): FPS, 1% low, frame-time graph, CPU time
  of the GPU command thread, GPU busy time from timestamp queries around every submission, GPU
  utilisation (NVML / PDH), VRAM, CPU, RAM, pipeline compiles, the busiest threads. A measurement (F9,
  the menu, or `measure=N` in the scripted pad file) writes `logs/perf-*` and `scripts/perf_report.py`
  turns it into a report. Queries are reset from the host before reuse: resetting them inside the command
  buffer let stale results read as available.
- **Present mode**: logged (`GPU: Present mode`). Mailbox, Fifo and Immediate give the same frame times
  on Hokuto: the drops are frame work, not presentation.

## Benchmark

`scripts/bench_hokuto.py record|walk|still`: a fresh copy of the data per run, Cross pressed until the
save list (Continue), gameplay detected from the frame statistics (the loading screen runs at a fixed
20 FPS), the recorded pad route (`PGG_PAD_RECORD` / `PGG_PAD_REPLAY`) and the measurement started on the
same frame, the game closed like its X. Standing: within 3% over three runs; walking: FPS and p99
within 2–5%, CPU and GPU time per frame vary more (the replay is time-based and the path can drift).
