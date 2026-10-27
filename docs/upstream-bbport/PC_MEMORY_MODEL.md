# The PC memory model, and the CPU and shader cache work around it (bbport 0.3–0.4)

English translation of the memory model, VRAM, GPU hang, weak CPU and shader cache sections of
bbport's `CHANGES_2026-10-06.md` (0.3, 4d4d208) and `CHANGES_0.4.md` (0.4, daa7165), merged in that
order. Upscaler, launcher, Linux packaging and Bloodborne patch notes are left out. Measurements are
bbport's, on Bloodborne (CUSA03173 1.09): RX 7800 XT / RADV, Mesa 26, 1080p output without an FPS
limit, unless stated otherwise; "4 cores" means `CPUS=0-3,8-11` (4 cores / 8 threads, like the Steam
Deck). The Linux paths (dma-buf, `BB_UFFD`) are bbport's; on Windows PortGaGotoku uses its own.

## 0.3: the PC memory model, experimental, off by default

Enabled by the launcher's "New memory model (experimental)" switch or `BB_GUEST_IN_PLACE=1`; by
default model 0.2 runs. Tested only on an RX 7800 XT and the Steam Deck, may crash; on NVIDIA dma-buf
cannot be mapped at an offset, and the port falls back to the old model. The game's direct memory is
allocated through Vulkan (dma-buf, GPU-visible system memory), and the GPU reads it in place, like a
PC game's buffers. There are no more copies of all memory in VRAM and no tracking of writes through
page protection. Readiness labels (EOP/EOS) are written when the GPU has actually done the work.

- Frames overlap: the first two command submissions of a frame (`BB_SUBMIT_AHEAD=2`) do not wait for
  the end of the previous frame. With K ≥ 8 the game's heap leaks (~2 objects per frame), so there
  is a watchdog: growth of 100,000 live allocations turns overlapping off for the rest of the
  session.
- Command submission is asynchronous (`BB_ASYNC_SUBMIT`): the segments' command buffers are closed
  by their recording threads, and the submit itself is an ordered task on those same threads.
- GPU signals accumulate in the draw recording stage and are published in a batch; ConstRam waits
  are targeted.
- Measurements, Yahar'gul, standing: old model 138.7 → 143.6 FPS, new 101 → 152–155 FPS. A heavy spot
  (~2000 draws per frame): 95.5 → 115–124 FPS; there the limit is now the draw recording stage.

The settings keys `pc_memory` and `old_memory_model` are no longer read: the new key
`new_memory_model` is off for everyone.

## 0.3: video memory no longer grows from area to area

Before the fixes VRAM grew from 3.8 to 7.3 GB in 13 minutes of play; now it stays at 4–5.5 GB.

- **The texture collector freed nothing.** The ten oldest textures the GPU rendered to used up the
  whole budget of a pass. Now such textures are skipped and go to the back of the queue; without
  memory pressure, textures not used for `BB_GC_IDLE_SECONDS` (20) seconds are evicted. Age is
  counted in seconds: the previous 16 command submissions (~3 frames) threw away everything that left
  the screen for a moment.
- **Blocks of game memory in VRAM were never freed.** Now blocks not used for `BB_VRAM_IDLE_SECONDS`
  (60) seconds go back to game memory and are moved into VRAM again on access (without that, a drop
  from 90 to 55 FPS: the GPU read them over PCIe). Blocks of memory the game unmaps are freed. Free
  slots are used by multi-block requests too; new blocks take the fullest 64 MB chunks, nearly empty
  ones are evicted, empty ones are returned to the driver.
- VMA takes memory in 64 MB blocks instead of 256 (`BB_VMA_BLOCK_MB`); textures of 4 MB or more get a
  dedicated allocation (`BB_VMA_DEDICATED_MB`): deleted textures do not hold large blocks.
- Frame statistics: the `Memory:` line shows the game's VRAM and GTT as the kernel reports them, RSS,
  VMA blocks, all Vulkan images, the texture cache, game memory blocks in VRAM, and the collector's
  work. `tools/load_watch.sh` writes VRAM/GTT/RSS every 5 s.

## 0.3: GPU hangs, the place found

GPU markers (`vk_breadcrumbs.*`, `VK_AMD_buffer_marker`; `BB_BREADCRUMBS=0` turns them off): before
and after every draw, dispatch and port pass, the GPU writes the command's number. On device loss the
log names the command the GPU stopped at, its shaders and parameters.

Found (a ~20 minute session): a hang in an indirect dispatch of the particle shader `cs 2da7fe60`
right after `cs 4ca76892` (1×1×1, the emitters' random number generator, a counter in GDS). The loop
counts of `2da7fe60` come from memory. Indirect dispatch arguments are now copied by the GPU into a
CPU-visible buffer: the report shows what the GPU read, and the statistics the largest indirect
dispatches. **The cause is not fixed yet**: the next occurrence with this data is needed.

A hang no longer looks like a silent frozen picture with the sound still playing (Steam Deck, the
Gehrman fight): GPU waits notice device loss, and a watchdog, after 10 s without GPU progress, writes
the marker report to the log and stops the game with a clear error.

## 0.3: crashes at start and in play

- Guest fault `0x263b8e7` (frequent on the Steam Deck): EOP labels were written with `memcpy`, which
  writes 4–7 bytes as two overlapping words. The game managed to free the block between them. Now
  labels are written with a single atomic store (`store_once`, `StoreLabel`). Not checked on the Deck.
- NVIDIA crashed at start with the new memory model: the driver does not map dma-buf at an offset.
  Before enabling the model the port checks this and, if it fails, stays on the old model.

## 0.4: the PC memory model is the default again, NVIDIA included

- `BB_GUEST_IN_PLACE=1` by default (`run.sh`). In the launcher, instead of "New memory model", an
  *Old memory model* switch (key `legacy_memory_model`): model 0.2 as a fallback.
- NVIDIA maps dma-buf only at offset 0. Before, the port then went back to the old model; now with
  such drivers every allocation of the game's direct memory is a separate Vulkan memory chunk (the
  game makes 14 allocations and maps each of them once, whole, at offset 0). The check at start
  prints "the driver's dma-buf maps at offset 0 only: one chunk per direct memory allocation".
  `BB_GUEST_WHOLE_CHUNKS=1` enables this mode on other drivers too (for testing without NVIDIA): on
  an RX 7800 XT 188–190 FPS, picture correct. **Not tested on NVIDIA.**

## 0.4: faster on weak CPUs

- `WRITE_DATA` and DMA waited for all deferred host copies (22 µs for each `WRITE_DATA`, ~35 per
  frame, 12% of the draw recording thread's time on 4 cores). Now they wait only for copies that read
  the range being written (`Scheduler::WaitHostCopiesFor`, a table of sources per 64 KiB); the PS4
  command processor does not wait for earlier draws before such a write either. Waits: 37 → 2 per
  frame. `BB_HOST_COPY_WAITS=all` restores the previous behaviour.
- The check for concurrent writes into the scheduler (`ProducerScope`) is enabled only with
  `BB_PRODUCER_CHECK=1` (launcher: *Crash diagnostics*).
- 4 cores: 158 → 167 FPS.
- `BB_SECTIONS=1` also shows stage B's tasks by function.

Comparison with 0.3: *Synchronisation as in 0.3* (`BB_AS_0_3=1`) brings back what changed after that
release: the old memory model, `WRITE_DATA`/DMA waiting for all host copies
(`BB_HOST_COPY_WAITS=all`), the concurrent write check (`BB_PRODUCER_CHECK=1`). Yahar'gul standing,
1080p: 123 FPS against ~186 with the new memory model and translation.

## 0.4: depth of field works again (issue #44)

With DoF and any upscaler, dark rectangles with sharp edges, stripes and an upside-down copy of the
scene appeared in the far blur (sky, fog); 0.2 did not have this. The cause was 0.3's targeted waits
on constant dumps (`DumpConstRam` waits for the draw recording thread only if it overlaps its reads):
only constant buffers went into the list of reads, but the recording thread also reads vertices and
indices from game memory. The game writes small draw data (the DoF full-screen quads among them)
through the constant engine, and the dump for the next draws overwrote it before it was read. The
window opened when the recording thread was delayed right before the DoF passes. Now vertex and index
ranges are in the list too (unprepared draws wait entirely). Waits went from ~150/s to ~850/s, FPS
unchanged in the places checked. `BB_CONSTRAM_PIPE=0` waits fully on every dump (A/B).

## 0.4: the shader cache no longer breaks the start (issues #28, #38, #39)

- Cache files are written under a temporary name and renamed: a crash or a power cut leaves the old
  file or none, never a truncated one.
- A truncated entry no longer stops the game when read (`Invalid or corrupted deserialization
  container`, code 23): damaged entries are counted and the cache is rebuilt. The same for SPIR-V
  with a bad header and for pipelines the driver rejected when preloading. SRT walker code from the
  cache is registered only after its length is checked.

## 0.4: the draw recording thread does not wait for the command recording threads (2026-10-07)

- The presenter (frame barrier, FSR 1, post-processing) is recorded like the draws, on the command
  recording threads. Before, it took the command buffer directly: the draw thread waited until the
  recording threads finished everything handed to them (~90 µs every frame) and submitted the frame
  itself (~45 µs).
- Where commands are still written directly (the upscaler passes), they go into a separate segment
  with its own command pool, after the handed-over segments, without waiting
  (`BB_DIRECT_SEGMENT=0`: waiting, as before). Signals and ordered tasks in such a segment stay
  queued, so as not to overtake earlier labels.
- The present thread waits until the frame's submit (from the recording thread) has reached the
  queue before putting its own wait on it: otherwise one queue would wait for a later submit (a GPU
  hang at start, found during this work).
- DMA into memory the GPU uses in place (PC model) or that the GPU wrote is a GPU copy in stream
  order; the draw thread no longer waits for copies and deferred signals before it (~16 µs each,
  ~900 per second: 4-byte GDS counters).

Measurements (Hunter's Dream, standing, 4 cores / 8 threads, PC model): 143–144 → 146 FPS; the draw
thread no longer blocks on command recording (it was 1.8–3.3% of the time). 16 threads: PC model
~186, old model 144–151 (±3% between runs, no difference from the previous build).

## 0.4: BB_READBACKS=2 no longer hangs at start

The exact readback mode protects pages the GPU wrote against reads too. The port's copy threads read
them as well (upload to VRAM: `bb:Copy*`, `bb:VkRecorder`): the read faulted into the fault handler,
which queued a readback to the GPU thread and waited, while the GPU thread waited for the draw
recording thread, which waited for those copies. Now the port's copies in exact mode read game memory
through a second mapping of the same pages, which the protection does not touch. Checked: with
`BB_READBACKS=2` the game reaches the title screen and runs (before it stopped while loading). Not
checked in play in this mode.
