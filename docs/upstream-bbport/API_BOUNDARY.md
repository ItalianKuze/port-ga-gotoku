# Moving away from emulation: what was done and what comes next (2026-10-06)

English translation of bbport's `EMULATION_REMOVAL_PLAN.ru.md` (bbport 0.4, daa7165). It describes
Bloodborne (CUSA03173 1.09); the boundary between the runtime and the GPU emulation is the one
PortGaGotoku uses. The libGnm hooks it mentions (`BB_GNM_OBSERVE`) were built on Bloodborne's eboot
offsets and have been removed from PortGaGotoku.

## Where the API boundary lies

Bloodborne is built with a static libGnm. The PM4 packets are written not by the driver but by the
game itself, through the `sce::Gnm::*CommandBuffer` functions (`0x1068000–0x10a0000`,
`0x11fd000–0x11fe000`). Hooking all of these functions (`BB_GNM_OBSERVE=1`) showed that 100% of the
submitted packets are written by known API calls, 99 methods in all. Gnm is an API that writes
registers: `setDepthStencilControl` = one `SET_CONTEXT_REG`. So parsing the PM4 from these 99
functions and translating the calls themselves give the same result; here PM4 is a recording format
for the calls, like a command list. It is already parsed in parallel (DrawPreparation).

Hardware emulation is not packet parsing, but what is done in place of the GPU:

| What | State |
|---|---|
| Memory model 0.2 (copies in VRAM, page protection, readbacks, early labels) | Fallback. The PC model is the default, with "one chunk per allocation" on NVIDIA. Remove the 0.2 code once checked on NVIDIA. |
| EOP/EOS labels | PC model: written once the GPU has finished the work (honestly). |
| `WaitRegMem` on labels of the same queue | Translated as a satisfied wait (queue order); the CPU does not need to wait. |
| `WRITE_DATA`/DMA waited for all host copies | They wait only for overlapping copies (like the command processor, which does not wait for shaders). |
| Constant Engine (CE RAM, dumps, counters) | Run on the CPU in stream order: our parser reads descriptors from memory. |
| SRT walker (GCN shader code run on the CPU) | Needed for the same reason. |
| Tracking GPU writes for the parser (`PendingWriteOverlaps`, `IsRegionGpuModified`) | ~15% of the GpuComm thread, same reason. |
| Texture cache by address + hash | Same reason, plus PM4 has no "texture" objects. |

Almost all the remaining emulation exists because **the translator reads the resource descriptors
(T#/V#/S#) from the game's memory on the CPU while parsing**. A PC driver and DXVK do not do this: the
GPU reads the application's memory while executing.

## Next, in order

1. **NVIDIA.** Test build `dist/Bloodborne-bbport-x86_64-test-1006.AppImage` (PC model by default,
   "one chunk per allocation" mode). If it works, remove the 0.2 model entirely (`page_manager`,
   `fault_manager`, readbacks, BB_UFFD, preloading, early labels).
2. **Parallelise stage B** (draw recording; 99% busy on 4 cores, ~3.3 µs per draw, spread evenly over
   buffer/texture binding, render pass begin, vertices). Options:
   (a) several stage B threads over draw ranges within a render pass (each with its own command
   segment; the shared caches are already under locks; barrier order by pass);
   (b) resolve bindings (buffers/textures → Vulkan) in stage A, which is 28% idle, with B only
   recording. Walking tests needed.
3. **Descriptors on the GPU (the main step towards "like DXVK").** The shader reads T#/V#/S# itself
   (in-place buffers and BDA already exist); textures are a bindless array, T# → index through a
   table on the GPU kept by the texture cache when it creates images. Then the SRT walker, the CE on
   the CPU and the tracking of pending writes are no longer needed; `WRITE_DATA`/DMA/CE dumps become
   GPU writes in stream order. A large project.
4. **Textures as objects.** Hook `Gnm::Texture::init*` and the TPF loader
   (`CSGXTextureCreateFragment`): the image is created at load time instead of being guessed from its
   address; hashes and address overlaps go away. Assets could be converted to linear layout ahead of
   time (detiling once, at install).
5. **Shaders ahead of time.** All GCN shaders from the game files → SPIR-V at install; no compiling
   during play.

What stays translation, as in DXVK: GCN shaders → SPIR-V, descriptors and formats → Vulkan.

## Step 1 (2026-10-07): the constant engine no longer synchronises with draw recording

Measurements (`BB_CE_STATS=1`, Yahar'gul): ~14,000 `DumpConstRam` per second, ~4–5 MB/s, 25 pages in
all (a ~100 KB ring). Each dump is read by exactly one pointer from user data: these are descriptor
tables for the SRT walker. Constants, vertices, indices and large buffers are not read from these
pages; there are no shaders that read memory on their own (`uses_dma`). So the CE dumps are needed
only by the translator: they build descriptor tables, they are not data for the GPU.

Stage A's waits on dumps (`WaitForPendingReads`) were needed only because stage B, for draws without
preparation, re-read the V# tables (vertex buffer descriptors) itself, by then overwritten by the
next dump (~15% of the V# belonged to someone else: the DoF bug, issue #44).

Done:
- stage A captures the V# into the draw packet, stage B takes them from there and does not read the
  tables;
- a DMA source is marked with its exact range instead of "reads who knows what".

Result: waits on dumps ~850/s → ~46/s, stage A waits 1.5% of the time instead of ~30% (16 threads).
On 4 cores / 8 threads the FPS is the same (~170): there the bottleneck is stage B (3.4 µs per packet;
the most is `ObtainBuffer`, 0.57 µs: matching game memory addresses to GPU buffers).

Command processor writes (`BB_CE_STATS`): `WRITE_DATA` ~8,400/s into 2 pages (read as constants
~240/s), DMA ~1,000/s into 15 pages. They write data for the GPU, and their translation is GPU
commands in stream order (`vkCmdUpdateBuffer`; DMA to in-place memory already works that way). For
`WRITE_DATA` this runs into the rule that moves blocks the GPU writes often into VRAM: to be done
together with simplifying the memory model.

## Step 2 (2026-10-07): stage B synchronisation

Stage B (draw recording) is the bottleneck on 4 cores (99% busy). Its per-frame tasks:
- the flip signal = the presenter preparing the frame; 600 k cycles per frame, of which ~40 k are
  work and the rest waiting for the command recording threads (`CommandBuffer()`) and a synchronous
  submit. Now ~50 k: the presenter is recorded through `Record`, the submit is asynchronous;
- the upscaler passes write commands directly: they get a separate segment, without waiting;
- DMA (~900/s, 4 bytes) waited for copies and signals even when the GPU does the write: now they
  wait only for CPU writes.

What is left on stage B (µs per packet, 3.5 in all): resource binding 1.0 (buffers 0.4, textures
0.45), `ObtainBuffer` 0.6, render pass begin 0.26, vertices 0.34, tasks 0.2. Next: splitting stage B
or binding resources in stage A (see item 2 above).
