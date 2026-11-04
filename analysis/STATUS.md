# Uncharted compatibility work — 13 October 2026

Target: CUSA02344 v01.00, eboot.bin (Drake’s Fortune).

The game boots through the Collection menu and opening movie into gameplay.
The user confirms normal controls and working audio after the clock correction.
3D appeared briefly, but movement led to GPU device loss. Work is incomplete.

Current investigation: dynamic shader resource reads. The GOW3 default disables
GPU direct memory access and substitutes flat-buffer data for unflattened reads.
The latest run enables GOW3_DIRECT_MEMORY_ACCESS=1; outcome is not yet verified.

Implemented: package/metadata extraction; three-executable preparation; full
fiber context switching; FFmpeg Videodec2; memory range names; dynamic GPU hook
queue; installed PlayGo metadata; disk space; offline services; concurrent event
queue lifetime safety; interrupt registry; optional executable placement below
2 GiB; buffered controller reports; optional native CPU clock consistency.

Clock tests passed in both modes. This CPU runs at about 4.3 GHz, whereas the
base runtime reports 1 GHz. Uncharted executes inline RDTSC instructions; the
launcher now measures and reports the matching hardware frequency.

Shader changes: support uniform ReadFirstLane resource offsets, balanced walker
stack on failed pointer offsets, bounded GPU semaphore diagnostics with explicit
error handling. Dynamic Phi/lane-dependent resource offsets remain unsupported
by the CPU walker; the GPU memory-read path is being tested.

Settings: vsync and 60 FPS; background shader compilation, draw pipeline, Vulkan
recording threads, draw preparation, prepared resources, deferred stream copies
and parallel copies disabled during diagnosis.

Launch: ../Start-Uncharted.cmd → tools/probe_uncharted.py →
gow3_pc/out/gow3-probe.exe. Game data: game/CUSA02344/uroot. No timeout.

Native tests passed for fibers, memory names/notification ordering, services,
content, concurrent handles, video decode, buffered pad and clock timing.
Windows build passes. Uncharted 2/3 gameplay has not been tested.

Logs, attempts and frame captures: uncharted/. Thread snapshot: thread-sample.json.
Older import counts predate later API implementations. REA opened the reconstructed
ELF, but decompilation was unavailable with the installed Windows providers.

## Update 13 October 2026 (evening): gameplay reached

The device loss on the way into gameplay (menu blur / loading, cutscene skip) is fixed.
Cause: TextureCache::FindDepthTarget linked any image starting at the depth buffer's
stencil address as its stencil proxy. Uncharted reuses that memory for a 960x540 RGBA8
texture; sampling it was redirected to the 960x540 D32S8 depth image and the GPU read past
that image's memory (VK_EXT_device_fault: ReadInvalid page fault). Only 8-bit images or the
format-less placeholder are now linked, and GetAssociatedDepth drops stale links.
The user confirmed gameplay after the fix.

Also fixed: evicted samplers are destroyed only after the GPU passed them (deferred).

Diagnostics (GOW3_GPU_CHECKPOINTS=1, set by tools/probe_uncharted.py; =2 adds
CommandBuffer() caller markers): on device loss the log shows the device fault report,
the draws/dispatches with their bound images and buffers after the last completed marker,
and which Vulkan object/guest image owns the fault address (device address binding report
plus image/buffer placement records). Diagnostic-only switches: GOW3_KEEP_FREED_IMAGES=1,
GOW3_FEEDBACK_LOOP_LAYOUT=0. The probe still disables several GOW3 performance features.

## Update 14 October 2026

- FrameBegin assertion (fg-draw-mgr.cpp:234, m_gfxEopTick of frame N-3): the game's wait for a
  frame's GPU end-of-pipe stamp (0x3fd6xx) gives up after 2.0 s. tools/probe_uncharted.py now
  NOPs that timeout (`ja` at 0x3fd816) in boot-linked.bin by unique pattern (EBOOT_PATCHES).
- Lighting: tiled lighting compute cs 0x43b8ee5e indexes 224-byte light records with a
  GPU-computed index (waterfall loop, v_readfirstlane); the SRT walker cannot pre-read them.
  GOW3_DIRECT_MEMORY_ACCESS=1 is now the probe default. Its per-light shadow texture
  descriptors (8 "not flatenned") remain unresolved.
- Jungle chapter (temple-0a/0b) once showed a grey world (level geometry, shadows and sky
  probe computes not rendered; Nate black) until a treasure pickup; all packs were read
  completely, PlayGo reports all 100 chunks available. After the new save it loads fine.
  Cause not established; watch for it on new chapter loads.
- Pipeline cache and background shader compilation are on again in the probe; GOW3 stale and
  deferred readbacks are off in analysis/uncharted-user/probe.ini.
- Diagnostics added: GOW3_FILE_TRACE=<text> (timestamped opens/reads), GOW3_CAPTURE_PASSES /
  number in the capture trigger file (frame capture of N passes), GOW3_SKIP_FILE (live list of
  shader hashes to skip), shader hashes in SRT/sharp warnings.
- Audio output timing is clean (no starvation); latency ~83 ms queued plus device buffer.
  Gunfire audio missing and black circles on bullet hits are still open.


## Diagnostic update 14 October 2026 (01:xx)

User-confirmed gunfire decrements ammunition but remains silent. Recorded input PCM on all eight main channels is zero during shots, before volume/downmix; port gains are nonzero, sound category gains are 1, and output never starves. Common/temple sound banks open and read successfully. Missing effects occur before SDL output conversion.

Shader compilation disabled and low renderer cache optimizations disabled did not recover the grey scene (mask restored to 159760). Native process counter now shares TSC units/frequency; clock tests pass, but user confirms this did not fix either symptom.

Compared Supermedo/bloodborne_pc windows commit e5d04e2 (see bloodborne-comparison.md). Ported cached SRT walker handler registration and speculative/concurrent fault safety, preserving local locking and balanced failed PushPtr. Build passes; scene remained grey. Shader binary/meta cache versions bumped 7 to 8 so earlier compiler output cannot silently survive updates. Latest test disables both pipeline and binary cache for fresh compilation.

One-shot guest image diagnostics added to Presenter: guest-images.trigger in GOW3_DUMP_DIR lists hex address and guest size; copies depth/stencil or uncompressed color. Only activates on frame-dump trigger. Intermediate capture shows depth containing props/characters, normals containing additional geometry, two lighting targets RGB zero. Capture files in uncharted/captures/1791923083373662700. New capture notes include precise per-draw depth/stencil flags. Root cause still open.

Latest follow-up: fresh shader test (pipeline and binary cache both disabled) still grey. Depth/stencil flags and stencil plane captured under uncharted/captures/1791923397807259500, pass notes in uncharted-user/captures/frame_20261014-013133_1.txt. Stencil has values 0/128/192/194/208/210, not entirely zero. Lighting output addresses rotate; cs43b8ee5e writes 0x11438f0000 in this capture, so zero 0x11408b0000 alone does not prove that compute failed. Presenter captures may observe a subsequent in-flight frame's depth; need capture at the specific rendering stage before attributing missing geometry to depth.

Corrected CLOCK_PROCTIME (ID15) to elapsed time since process start, rather than accumulated host CPU time. Regression tests pass default/native. Trace currently observes clock ID4 only; no evidence that ID15 caused current symptoms. Latest run session15495, capture folder1791923766600283800, still grey. Exe gow3_pc/out/gow3-probe.exe, launcher Start-Uncharted.cmd. No goal completion claimed.

## Update 14 October 2026 (later): fixes and the open "grey world" investigation

Fixed (in the build):
- Event queues (gpu/shadps4/core/libraries/kernel/equeue.cpp): the poll path (timeout 0)
  read and cleared triggered events without the queue lock, losing a concurrent trigger;
  waits with a pending small HR timer ignored other triggered events; TriggerEvent woke one
  waiter only (now notify_all).
- Shader cache key vs DMA: shaders compiled with GOW3_DIRECT_MEMORY_ACCESS=0 stay in the
  pipeline cache; the old cache was moved to analysis/uncharted-user/cache-before-1014.
- Evicted samplers are destroyed after the GPU passed them (texture_cache.cpp).
- Audio latency: queue target 1-2 periods (GOW3_AUDIO_QUEUE_BUFFERS, default 1) and a 480-frame
  WASAPI buffer request (GOW3_AUDIO_DEVICE_FRAMES): ~41 ms queued instead of ~83 ms, no starvation.

Diagnostics added:
- GOW3_FILE_TRACE=<text>: timestamped opens/reads/seeks; every failed open is now logged
  (errno, file table full, path translation).
- GOW3_AUDIO_TRACE=1: Ajm instance create/destroy always, RIFF jobs, 5 s decoder totals;
  sound1/ paths traced.
- GOW3_RETEXTURE_FILE=<file>: creating the file marks all CPU-sourced textures for re-upload.
- GOW3_SKIP_FILE, GOW3_CAPTURE_PASSES / pass count in the capture trigger file.
- Scratchpad tools (memory peek/watch/snapshot) read game globals at guest base 0xb400000.

Grey/black world after loading the jungle (intermittent, reproduced via main menu -> Continue):
- Workaround: Pause -> Treasures -> back (a plain pause or other menus do not help).
- The game renders reduced material variants: no sun shadow pass, no level G-buffer pass,
  no sky/ambient computes (cs 2a2e99be, 0a57ac2f, 43225bd9); ground flat or missing, Nate
  sometimes unlit.
- Texture streaming (texturedict1/*-tstrm.pak) reads stop right after the load and resume in
  a burst when Treasures opens. The streamer object (0xe66800) stops changing; its update gate
  flag (0x994188+0x39b7) stays 1.
- Ruled out: failed file I/O (all reads succeed), PlayGo (all 100 chunks available,
  0xd30df0), package loading (all 74 packages status 12, manager 0xd1b988), stale GPU
  textures (forced re-upload of 447 textures: no change), readback mode (Precise: no change),
  occlusion queries (always visible), Windows red-zone clobbering (measured intact on the
  Ryzen 9 9950X3D), SRT/DMA lighting.
- Correlated: the menu host (0xabb148) page stack top index (+0x790, pages at +0x590) is 2 in
  the broken state and 0 after the Treasures fix. Forcing it to 0 did not fix the world, so it
  is a symptom of the same condition.

Audio: only music plays in the broken state (level-driven sounds absent). Sound banks
(common.bnk etc., mono ATRAC9 RIFF) are opened; no bank voice reaches Ajm. Gunfire was the
only missing sound when the level loaded correctly.


## Grey/black ground investigation, 14 October 2026 (afternoon)

Ruled out (bad Continue load compared with the same load after Pause -> Treasures -> back):
- Texture streamer gates: manager 0xd60590 (dispatcher 0x3dfb90 registered at 0xe66710,
  mode 2 path 0x3e1b90). Enable byte +0xf1530 = 1, pending-op ring +0xf1558/+0xf1560 empty,
  streaming-state arg null. Needed-set +0x8c0 empty and request slots (0xe66800, stride 0x88,
  state +0x50) idle in good and bad loads alike, so the stall is a symptom (nothing is drawn,
  so nothing is requested).
- Rendered-background list (UpdateBackgroundsForRendering 0x3aeb10, manager 0xfdb5f0: list at
  +0x0, count +0x180): the same 4 level backgrounds in good and bad loads. Their objects and
  all objects they point to are unchanged by the Treasures fix (only a camera-following
  transform differs; +0x80 is a "transform changed" flag, set at 0x3afdcb).
- Debug flag "Draw Levels Outside Vis" (0x98f8e0+0xc5) forced on: ground still black.
- FgDrawMgr (0xd5c500) slot bitsets: no difference. Thread stacks: no stuck thread.
- The Treasures screen registers a treasure model with FgDrawMgr (0x3c65a0) and loads two
  packages (package manager 0xd1b988 +0x100/+0x168 go 7 -> 9).

In the bad load, grass, rocks and characters draw but the ground under them is black, and
the fix is only partial (large black ground areas remain). Next step: the frame capture now
records indirect draws and indirect dispatches, with arguments as the CPU sees them
(vk_rasterizer.cpp DrawIndirectRecord/DispatchIndirectRecord); earlier captures omitted
them, so "no level G-buffer pass" may have been indirect draws that were not recorded.
Launch with GOW3_CAPTURE_TRIGGER=<file> (write a pass count into the file) or
GOW3_FRAME_CAPTURE_KEY=1 (F11).
Tools: scratchpad heapsnap.py (full guest memory save/diff in ~5 s), threads.py (guest stack
sampler), bgdump.py.

### Capture comparison, same load, bad vs after Treasures fix (10:40 / 10:42)
Captures: uncharted-user/captures/frame_20261014-104041_1.txt (bad) and
frame_20261014-104215_2.txt (fixed), 600 passes each (GOW3_CAPTURE_TRIGGER with a pass count).
- No indirect draws or dispatches at all (new capture notes would show them).
- Both frames issue the same passes and draws (7602 vs 7612 draws, 78 computes, same shaders).
  The ground IS drawn in the bad state; it renders black. Earlier "no level G-buffer pass"
  conclusion is wrong for this repro.
- Frame structure: depth prepass interleaved with one-draw depth-only test passes (ps d3cd03d8 /
  bb6ab1e5, sampling an R8 64x64) = occlusion-query boxes; ZPASS_DONE results are synthesized
  as "visible" (liverpool.cpp EventWrite), SET_PREDICATION is ignored.
- Only sampled-texture difference: 4 streamed textures. Bad: 0x11547a0000 BC5 512, 0x1155038000
  BC1 sRGB 512, 0x1155064000 BC1 512, 0x115853c000 BC1 64. Fixed: 1024x1024 versions at new
  addresses (0x115ef84000, 0x115f938000, 0x11603bc000, 0x1164578000, 0x116478c000).
- Working hypothesis: the low-mip ground textures hold stale/black content in the texture cache
  (filled by a path the cache does not observe, e.g. GPU/DMA copy, or marked GpuModified so the
  forced re-upload skipped them); the Treasures fix streams in higher mips at new addresses,
  which are created fresh. Next: dump guest bytes at those addresses vs cached image contents.


### Review and texture evidence, 14 October 2026 (10:50)

Reviewed the 10:40/10:42 comparison. Important limitation: vk_frame_capture.cpp
AddSampled keeps only the first 32 unique sampled/storage textures per pass;
AddShader keeps only 8 shader hashes. Thus "only sampled-texture difference: 4"
is a bounded observation, not a complete comparison of all materials/shaders.

Added one-shot BC1/BC5 cached-image dumps (base mip, layer 0) to the existing
Presenter guest-images.trigger diagnostic, plus guest layout/flags metadata.
It does NOT call UpdateImage, avoiding an accidental refresh before capture.
Build passed. Latest running capture folder: uncharted/captures/1791956845059173200,
pass capture uncharted-user/captures/frame_20261014-104946_2.txt. This save resumes
near a rock arch and still has missing/grey terrain and black Nate.

Actual decoded GPU images for the corresponding four current-run addresses:
- 0x115478c000 BC5 512: pistol normal map.
- 0x1155024000 BC1 sRGB 512: pistol colour/material atlas.
- 0x1155050000 BC1 512: pistol material mask/atlas.
- 0x115852c000 BC1 64: small pistol-associated mask (sampled alongside the above).
The former 10:40 addresses differ by allocation placement in this run. These
captures do not support attributing those four texture changes to the ground.
See texture-comparison.png (GPU left, CPU micro-detile interpretation right).

Read-only guest snapshots: tools/snapshot_guest_textures.py <capture-directory>.
Comparing base-mip compressed-block multisets removes ordering/tiling as a source
of false mismatches: the four suspect textures differ from current CPU bytes;
other captured textures match exactly (0x11549f4000, 0x1154a00000, 0x1154ecc000).
All nine captured images have flags 0x40 (Registered, no Dirty/GpuModified/Aliased).
GPU pistol images are recognizable and nonblack; CPU bytes may have been reused
or overwritten. A mismatch is observed, but does not yet establish which copy is
correct or explain the missing ground. Do NOT force arbitrary CPU re-uploads as
a fix based on this comparison alone.

Earlier opt-in GOW3_DEPTH_PROBE_FILE diagnostic remains available (0 guest,
1 Equal->LessOrEqual, 2 Equal->Always, only draws without depth writes). It has
NOT been tested in gameplay and is not enabled in this run. No graphics or audio
fix is claimed from this review.

### Main-pass buffer comparison, 14 October 2026 (11:07, reviewed later)
analysis/bad-mainpass.txt vs good-mainpass.txt (pass #154, 795 draws, same shaders):
- Of 17 recorded buffers, exactly one differs zero/non-zero: the first draw (vs da3d9c98 /
  ps 45d0299c) slot 1, 42616 bytes, 28-byte vertex stride (positions about -8..9). Bad: all
  zeros (0x11009cd520). Good: filled (0x1100ecd520).
- The same draw's slot 0 constants [158..170] are zero in bad, and in good hold
  1,1,1, 107/255,88/255,73/255, 5,25,4,60, 0.2,0,1 (looks like a colour plus parameters).
- The vertex data is not stored verbatim in temple-0*/common packs, so it is generated per frame.
- tools/scan_ground_state.py (read-only) searches live memory for both patterns -> analysis/ground-scan.log.

### Ported from the MediEvil port (reference/medievil, fork base ce1886a), 14 October 2026 17:28
- (reverted after the 17:47 Fios EIO crash) runtime_mutex.c retirement of destroyed mutexes/conds;
  a destroy of a held mutex returns EBUSY.
- Depth bias constant scaled by PA_SU_POLY_OFFSET_DB_FMT_CNTL (regs.h 0x2DE) vs the host depth
  format; the log prints "GPU: depth bias ... scaled" once if it ever applies.
Build passes. No mutex unit test exists. Not relevant to the grey ground: MediEvil's CMask fill
(Uncharted never logs "Unhandled metadata") and memory-pool APIs (not imported by Uncharted).

## Grey world: cause found, fix pending test (14 October 2026, evening)
Live bad state (after main menu -> Continue): menu host 0xabb148 page stack top (+0x790) = 2,
stack = slot +0x3b0, slot +0x3c0, slot +0x448. +0x448 (and its twin +0x440) is the one-button
message page; descriptor host+0x8f0 = message 0x155d235e, button 0xc6467f2d, action 0x2dac.
Code (llvm-objdump of analysis/uncharted/eboot.elf; REA cannot open this ELF on Windows):
- Menu step 0x14a8b0 (also 0x149698, 0x14a986): if 0x577b60(&0x11a69d0) is false, toggle
  0xabbaf4 and push message 0x155d235e on page +0x448 or +0x440 (alternating). OK (0x2dac)
  goes to menu state 6, the same as the "ready" path (0x14aa2c).
- 0x577b60: true if flag 0x994188+0x3a04 == 0 (a general flag, read by the renderer too),
  otherwise object state (+8) not 0 and not 2. Object 0x11a69d0 is the PSN TUS client
  (its state machine near 0x578000 calls sceNpTus*/np_signed_out through GOT 0x9875d0..0x987608).
  Signed out, state stays 0.
- The message page stays on the stack in gameplay, so menu overrides persist: level sounds muted,
  level lighting constants zero (ps 45d0299c [158..170]), ground vertex buffer empty.
  Pause -> Treasures -> back clears the stack.
Fix: tools/probe_uncharted.py EBOOT_PATCHES makes 0x577b60 return true (b0 01 c3). Other callers
(0x149083, 0x14ca5c, 0x14d160) also require +0x28c4 ("TUS data loaded"), which stays 0.
Not yet tested in game. Test: Continue at least 3 times from the main menu; check the ground,
level sounds, and that the page stack top (image 0xabb8d8) is 0.
- 17:47 run with the PSN patch plus the MediEvil lock port crashed during the temple-0a -> 0b load:
  ASSERTION ErrFromFios (ndlib/io/streaming.cpp:523), SCE_KERNEL_ERROR_EIO, fault 0x395ac2.
  First occurrence. The lock port is reverted: its cond_destroy always destroyed even with waiters,
  while the PS4 returns EBUSY there, so a Fios wakeup could be lost. Depth-bias port and PSN
  patch kept. Rebuilt 17:5x.
- PSN patch tested 17:5x: popup gone, world still grey. Menu stack then [+0x3b0, +0x3c0], top 1.
  The menu host mode (+0x0; 1 = front end, 2 = in-game; IsFrontEnd 0x14adf0 = mode < 2) is 1 on bad
  loads and 2 after Treasures (0x14b2e0 opens the in-game menu: SetMode 0x148630(2) and replaces the
  stack with slot +0x468). Writing mode 1 into a good state changed nothing visible, so it's a symptom.
  PSN patch reverted.
- Key evidence (same session, 11:06:15 bad vs 11:06:59 after Treasures): the ground draws (vs
  823cb43a pass #137, vs da3d9c98 pass #154) read a 42616-byte vertex buffer that is all zeros in
  guest memory at 0x11009cd520. After Treasures the same mesh is filled at 0x1100ecd520 (+5 MB),
  and the SRT constants [34..37] (25, 4, 60, 0.2) of ps af7700b5 are filled too. So level geometry/data is
  allocated but never filled on bad loads; Treasures reloads it. The fixed state still shows grey holes
  in the distance (screenshot via PrintWindow).
  Suspect: level data streaming via Fios (ndlib/io/streaming.cpp; the same code asserted EIO at 17:47).
- Next run: GOW3_FILE_TRACE=.pak (probe default for now, 200000-line cap) to see whether pak/stream
  reads stop or fail on a bad load. tools/globals_diff.py (image globals save/diff) and
  tools/peek_any.py added.

### 14 October 2026 (evening): ground geometry is not generated on bad loads
- Reference review: reference/bloodborne (Supermedo windows branch) and reference/shadps4 (sparse,
  kernel/fiber) show no file, lock or DMA fix that applies. Our DMA CopyBuffer matches upstream, and
  FindImageFromRange already defaults to ensure_valid.
- .pak trace (GOW3_FILE_TRACE): every read succeeds; temple-0a/0b are read completely; streaming reads
  stop after the load.
- Per-frame GPU ring at 0x1100000000 (1 MB per frame slot; tools/region_map.py): bad frames write about
  0x90000 bytes, good frames 0xC0000. The extra data is ground vertices (stride 28, constant 0.00785
  before each position), so the game's CPU code doesn't emit the ground on bad loads.
- Menu host forced to the good configuration (mode 2, stack [+0x3b8], top 0) in a bad state:
  still black. Menu state is a symptom.
- Globals diff, two sessions (tools/globals_diff.py, analysis/globals/*): 60 consistent differences,
  e.g. allocation bitmaps of the object at 0xe924f0 (vtable 0x982810, methods 0x73xxxx/0x74xxxx,
  block 0xe92a10 size 0x5860) all free on bad loads; 0xe65190/0xe652e0 0 vs -2.
- Next: find the ground vertex writer with a hardware write watchpoint in a good state.
  The runtime now logs "Watch:" lines (probe.c watch_hit); arm with tools/watch_write.py <addr...>,
  e.g. ring offsets +0xa0000 of the active slots. Profiles: tools/sample_guest.py + profile_report.py.
- Watchpoints (DR0-3 on all threads, slot +0xa0000 of the active 1 MB blocks at 0x1100x00000): no CPU
  write hits, before or during a second Treasures. WriteProcessMemory to these pages fails (GPU page
  tracking keeps them write-protected), so an earlier "value restored" test was invalid.
- Leading model: the ground vertices are produced on the GPU. cs 308607aa (18 dispatches per frame,
  groups 26x1x1) reads compressed meshes from package memory (0x119e...) and writes decompressed
  vertices into 0x1100...; captures listed only its first 4 dispatches. The ground draws (vs 823cb43a,
  vs da3d9c98) read 0x1100xcd520.
- Capture now lists every draw's and dispatch's buffer addresses with GOW3_CAPTURE_ALL_BUFFERS=1
  (probe default), triggered by writing a pass count into analysis/uncharted-user/capture.trigger.
  Next: bad vs good capture; check whether a 308607aa dispatch writes the range the ground draws read.
- 18:3x: the ground shader's lighting records (64-byte table at 0x1038ee8380.., referenced through
  V# 0x1038ee858d in ps 45d0299c's flattened SRT data; same address in every run) are zero in bad
  states. After Treasures, 0x1038ee8500 became 1,1,1, 0.42.. with no CPU write (DR watch armed), so
  the GPU writes this table. Our SRT walker reads it from guest memory on the CPU at draw recording:
  candidate CPU/GPU ordering or coherency bug.
  Also, GPU compute output (e.g. 7d7e7206, 308607aa) is not visible in guest memory, so zero vertex
  buffers in guest memory prove nothing.
- Added GOW3_WATCH_RANGE=lo:hi (gpu/shim/gow3_watch_range.h; probe default 1038e00000:1038f00000):
  logs storage-buffer binds, DMA copies (CPU/GPU path), fills and WRITE_DATA into the range.
  Build 18:3x installed; the running game used gow3-probe.1818.exe (renamed).
- 18:4x ROOT CAUSE CANDIDATE: GOW3_WATCH_RANGE shows shader 2d215c3b (storage slot 1, many 16-172-byte
  writes) writing the constant memory 0x1038e7.../0x1038eb... that the ground shaders' SRT data points
  into. GOW3_READBACKS defaults to Relaxed (emulator_settings.h): GPU-modified pages are not
  read-protected, and GPU data reaches guest memory only when the CPU writes that page (write-fault
  readback). The CPU-side SRT walker therefore flattens stale zeros: ground constants (transform and
  lighting) are zero, so the ground collapses and Nate is unlit. Treasures happens to cause a CPU write to
  the page, which triggers the readback.
  Test: GOW3_READBACKS=2 (Precise) as probe default. The earlier "Precise: no change" predates this
  evidence.
- 18:4x CONFIRMED: in a grey state (no menu use), copying the GPU-modified range 0x1038e00000-0x1038f00000
  back to guest memory (GOW3_READBACK_TRIGGER "1038e00000 1038f00000"; BufferCache::DownloadRanges)
  restored the ground and Nate's lighting at once. Cause: GPU-written constants (copy shader 2d215c3b),
  flattened by the CPU-side SRT walker from stale guest memory under Relaxed readbacks.
  Precise readbacks (GOW3_READBACKS=2) hang on Continue. Fix candidate: GOW3_SYNC_GPU_CONSTANTS
  (Rasterizer::SyncSmallGpuWrites copies storage writes of 4 KB or less back at each submission,
  merged into 64 KiB windows).

## FIXED 14 October 2026 (18:50): grey/black ground, unlit Nate
Cause: the game writes per-object constants (ground transform and lighting records in 0x1038e.....)
with a GPU copy shader (cs 2d215c3b). Draws read them through SRT pointers, which our CPU-side SRT
walker flattens from guest memory. With GOW3_READBACKS=Relaxed (default) GPU writes reach guest
memory only when the CPU later writes the same page, so the walker flattened zeros. Pause -> Treasures
-> back fixed it only because a CPU write landed on that page and caused the write-fault readback.
Fix: Rasterizer::SyncSmallGpuWrites (vk_rasterizer.cpp) plus BufferCache::DownloadRanges. Shader
storage writes of 4 KB or less are copied back to guest memory at each submission (64 KiB windows,
one GPU wait). GOW3_SYNC_GPU_CONSTANTS=1 is the probe default; it costs about 1 FPS (57-59 FPS).
GOW3_READBACK_TRIGGER file: "auto"/"off", or "<lo> <hi>" for a one-off range readback.
Verified: a live grey state was fixed by a one-off readback of 0x1038e00000-0x1038f00000, and auto
mode is on. Fresh Continue loads with auto mode from launch are still to be confirmed by the user.
Not fixed by this (open): silent gunfire, black bullet-hit circles, the Fios EIO crash seen once
(17:47).
Ruled out on the way: PSN TUS popup and menu host mode (symptoms), Precise readbacks (hang on load).
Diagnostics left in the code (off by default): GOW3_WATCH_RANGE, GOW3_CAPTURE_ALL_BUFFERS,
Watch: debug-register hit logging in probe.c (tools/watch_write.py), tools/globals_diff.py,
sample_guest.py, region_map.py, peek_any.py.

## Sound effects silent (gunfire, footsteps, ambience); music and dialogue fine (14 October 2026, evening)
- Audio settings in game: all volumes full, 2 ch TV. Menu host mode/Treasures make no difference.
- Engine: Sony Scream with the BRB2 synth (brbDriver thread); Bluepoint audio config manager
  (biglib\audio\acm.cpp). Only 4 Ajm instances (codec AT9, flags 0x41 x3, 0x9 x1) decode the
  streams (music 4 ch, speech 1 ch); bank voices never reach Ajm (27 RIFF jobs per session, none on
  gunfire). common.bnk is intact in memory at 0x1000000000.
- The sound config text (sound1/config/ps4/prototypes.txt, banks.txt, reverbs.txt, environments.txt,
  mixers.txt) is NEVER opened and is not in memory. The load runs at startup:
  0x9bba5 -> 0x522d80 (acm 0x104c210) -> 0x519320/0x519410/0x5193a0/0x5159a0 (prototypes.txt)
  -> 0x515a50 -> file object 0x10fddf8 (enabled, callbacks installed: open = 0x8ef80) -> path builder
  0x8dfa0 (mode 1 loose override is off: 0x994188+0x3990 = 0; mode 2 = <root>/build/main/sound1/
  config/ps4/<name>) -> game FS 0x393760 (Fios). Without prototypes, every sound-proto request
  (footsteps, weapons, ambience) resolves to nothing; streams don't use protos.
- Next run (build 19:07): GOW3_AUDIO_TRACE also logs stat/access of sound paths;
  GOW3_FILE_TRACE=sound1/config. Find where the config open fails.
- 19:2x CAUSE FOUND (startup instruction breakpoints, GOW3_EXEC_WATCH, probe.c exec_watch_thread):
  open callback 0x8ef80 is called for reverbs.txt, mixers.txt, environments.txt and prototypes.txt
  (flags 1, type 1, mode 0). The path builds fine, but the Fios open (0x393760 -> 0x771a50) returns
  0x80820015 (SCE_FIOS_ERROR_READ_ONLY), mapped to EROFS 0x1e: `testb $5,%cl` at 0x8f1da turns flag
  1 into O_CREAT (0x40 -> Fios O_CREAT 8) on read-only /app0. Fios fails it without any kernel call.
  Fix: EBOOT_PATCHES "sound config files open read-only" (f6 c1 05 -> f6 c1 04). To be tested in
  gameplay; GOW3_EXEC_WATCH=515cc9 logs the loader result (expect rax 0).

## FIXED 14 October 2026 (19:3x): sound effects (footsteps, ambience, gunfire)
Confirmed by the user in gameplay. Log: all 4 config files open (reverbs 11405, mixers 405,
environments 10291, prototypes 204474 bytes); loader result 0 for each; Ajm now decodes bank
voices (794 RIFF jobs vs 27 before).
Fix: EBOOT_PATCHES "sound config files open read-only" (0x8f1da testb $5 -> $4).
Diagnostics left in code, off by default: GOW3_AUDIO_TRACE (now also stat/access of sound paths),
GOW3_FILE_TRACE, GOW3_EXEC_WATCH (startup instruction breakpoints, up to 4 guest offsets; hits log
registers and strings at rsi/rdx).
Still open: black bullet-hit circles; the Fios EIO crash seen once (17:47).

## 14 October 2026 (19:4x): dark interiors - fire and flashlights add no light; hair and cobwebs bright
- In the temple interior the braziers light nothing around them, the torch barely lights the walls,
  and hair, cobwebs and vines look unshadowed or bright.
- Wider readbacks (0x1030000000-0x1040000000, 0x112d200000-0x112d300000) changed nothing.
- Lights: cs 9bd0e806 builds per-light data (32 KB at 0x112d220200, including indirect args for the
  flashlight volume draws vs 1bf62085 / ps fe99f21a, 8a04b99b; those sample a D32 2048 shadow map and a
  512x512 cookie). cs 43b8ee5e (tiled lighting) loads 8 T# from light records with a GPU-computed
  index; SharpLocationFromSource -> UNKNOWN_LOCATION -> Image::Null -> null descriptor reads 0, so those
  lights are fully shadowed.
- Change (build 19:4x): resource_patching_pass returns 1.0 for reads of unresolvable T#s (lights apply
  without their per-light shadows); shader cache versions 8 -> 9. To be tested.
- Result: no visible change. 43b8ee5e has a single unresolvable image (its 8 T# dwords). The change was
  reverted (shader cache versions -> 10).
- Further facts for the dark-interior issue (open):
  - Flashlight volume indirect args are valid on the GPU (0x112d220200: 1230 indices, 0x112d220220:
    1200; the CPU view is stale zero).
  - Shadow map 0x1137458000 (D32 2048) is rendered by pass #15 (131 draws, depth less, no clear in the
    frame).
  - Indirect barrier is correct (dst AllCommands, MemoryRead|Write).
  - Depth-bias scaling never triggers.
  - Next: dump the light accumulation targets (0x113f8b0000 and 0x11408b0000 R16G16B16A16, written by
    the volume passes #20/#22) and the shadow map in this scene (frame dump diagnostic,
    guest-images.trigger) to see where the flashlight light is lost.
- 19:3x image dumps (launcher now always sets GOW3_FRAME_DUMP_TRIGGER/GOW3_DUMP_DIR =
  analysis/uncharted/dumps; write guest-images.trigger "addr size" lines, then touch trigger):
  in the dark room, the flashlight volume pass (ps fe99f21a) samples a 256x8 R16G16B16A16 ramp/LUT at
  0x1134ab0000 that is ALL ZERO (texture cache image flags 0x40; guest memory zero; GPU buffers zero).
  The light accumulation targets 0x113f8b0000/0x11408b0000 and the tiled output 0x11438f0000 are zero
  too. Shadow map 0x1137458000 (depth 0.18-0.79), cookie and normals look valid. Nothing writes the LUT
  in any capture; it is built at load/startup.
- GOW3_WATCH_RANGE now also logs render targets (BeginRenderingFull) and storage images (BindTextures);
  probe default 1134a00000:1134b00000 to find the LUT writer on the next launch/load.
- 19:4x narrowed GOW3_WATCH_RANGE (1134ab0000:1134ad0000): NO GPU-side write (render target, storage
  buffer/image, DMA, fill, WRITE_DATA) to the LUT in a whole session (startup, load, dark room), and the
  LUT is still sampled at 0x1134ab0000 (ps fe99f21a). Direct memory aliases share one section, so
  aliasing is not the issue. Next: startup data watchpoints on the LUT texels (GOW3_EXEC_WATCH now
  accepts w:<absolute address>, 8-byte write watch; GOW3_EXEC_WATCH_SECONDS, default 600).

## 14 October 2026 (20:0x): Uncharted 2 and 3 from the Collection menu

Symptom: picking Uncharted 2 or 3 in the menu closed the game. Four earlier boot logs show
`STOP: first unsupported PS4 import: JoBqSQt1yyA#libSceSystemService`, caller 0x14c7fd.

Cause:
- The Collection is three executables. The menu (eboot.bin, also Drake's Fortune) calls
  `sceSystemServiceLoadExec("/app0/big2-ps4_Shipping.elf"` or `big3`, with argv
  `{"eboot.bin", "ispartofuccollection"}` (string table 0x7778b0/0x7778d0 and 0x7a95ba/0x7a95c4;
  `"simulateplaygo"` is appended when a debug flag is set).
- The runtime had no LoadExec, so it exited.

Changes (not yet tested in game; the game was running during the build):
- runtime_services.c `load_exec`:
  - Writes the path and arguments, one per line, to GOW3_LOADEXEC_FILE (analysis/uncharted-user/loadexec.txt).
  - Exits with code 30. Without a handoff file it exits with 31 and a STOP line.
- probe.c: new `--guest-path /app0/<exe>` and repeatable `--guest-arg` options build guest argv (previously fixed to `/app0/eboot.bin`).
- tools/probe_uncharted.py loops:
  - On exit 30 with a handoff file, it prepares the requested executable and relaunches with those arguments. The same path handles U2/U3 back to the menu.
  - U2/U3 started directly (Start-Uncharted2.cmd / Start-Uncharted3.cmd) get the menu's arguments.
- U2/U3-only imports, mapped in import_names.inc and implemented:
  - POSIX open/read/write/lseek/close/fsync through libScePosix (existing implementations).
  - fcntl, select and sockets (network down).
  - pthread_condattr_init, scePthreadGetthreadid.
  - NpSignaling GetConnectionInfo/TerminateConnection/GetPeerNetInfo, NpScorePollAsync.
  - A `--check-imports --cpu-only` diff now shows nothing missing in U2/U3 that is not also missing in U1.
- The 2 s GPU frame-wait patch also applies to U2 (image 0x718886) and U3 (0x79f316); the code around it is byte-identical to U1's.
- The U1 sound-config patch has no counterpart: U2/U3 have no prototypes.txt.
- The U1 dark-interior diagnostics (GOW3_WATCH_RANGE, GOW3_EXEC_WATCH) are only set for eboot.bin.

Result (14 October 2026, 20:2x):
- The first test showed a grey screen at 60 FPS. The game loop ran, but nothing loaded and every draw was a fullscreen pass.
- Cause: the launcher passed argv as `/app0/big2-ps4_Shipping.elf eboot.bin ispartofuccollection`.
  - The U2 option parser (0x637250, called from 0x14972c) appends unknown arguments to a list at +0x3a10 and clears +0x4975.
  - So "eboot.bin" was taken as something to load, and the title screen never started.
- LoadExec's argv is the new program's complete argv, with argv[0] "eboot.bin". The launcher now passes it that way.
- U2 and U3 both reach their "Press X to Begin" title screens at 60 FPS (no input sent).
- Gameplay is not tested yet.
- Diagnostics used, in the scratchpad: walk.py (thread frame chains), fibers.py (sceFiber objects and the call chain of each suspended fiber), busy.py (non-idle guest call chains).
  - U2's game loop runs as job 0x14e020; its frame counter is at image+0xebd4e0.

## 14 October 2026 (20:3x): Uncharted 2 new game froze on the loading screen (dagger icon)

The game loop stopped at frame 0x501. An lldb backtrace of all threads (`lldb -p PID --batch -o "thread backtrace all"`) showed a deadlock:
- The GPU command thread: Rasterizer::Draw → GetGraphicsPipeline → QueueShader → `capture_job.Wait()`.
- The ShaderCapture worker running that job: SRT walker (JIT) read of 0x1038e0cd80 (GPU-written constants) → read fault → Rasterizer::ReadMemory → BufferCache::ReadMemory → `liverpool->SendCommand<true>` (waits for the GPU thread).
- Every other thread that faulted, plus direct_unmap → UnmapMemory → InvalidateMemory, queued behind the GPU thread.

Fix:
- PipelineCache::capturing_for_gpu_thread is a thread_local, set inside a capture job that the GPU command thread waits on.
- Rasterizer::IsGpuSideThread() returns true for that thread, so the fault takes the inline readback path (assume_locks) that a GPU-thread fault would take.
- This is safe because the GPU thread is blocked in Wait() for the whole job.

Not yet confirmed in game.

Second hang (20:34, caught by scratchpad/hangwatch.py, which runs lldb when U2's frame counter stops for 8 s):
- A draw-preparation worker in GetProgram held `programs_mutex` (shared).
  - Inside its speculative section (runtime_fault_recover set), ParseFetchShader read a GPU-modified page.
  - The read fault went to gow3gpu_handle_fault → ReadMemory → SendCommand, which waits for the GPU thread.
- The GPU thread was waiting for `programs_mutex` exclusively (GetProgram line 1245).
- Fix (probe.c, both the Windows and the POSIX path): during a speculative read, a fault skips GPU page handling and takes the recovery path. The draw-prep result fails and the GPU thread does that draw itself.

## 14 October 2026 (20:4x): U1 dark interiors - lights FIXED by synchronous shader compilation

Cause:
- The 256x8 R16G16B16A16 LUT at 0x1134ab0000 (sampled by the flashlight volume pass, ps fe99f21a/8a04b99b) is rendered once at startup by ps 2d7325a4 (render target 128 KiB).
- With async_shaders=1, PipelineCache::GetGraphicsPipeline returns nullptr for a pipeline that is still compiling, and that draw is skipped.
- Pipelines are not warmed up at start ("on demand"), so this one-time draw was skipped every session.

Fix (tested by the user 20:4x: the torch and flashlight light the room):
- The launcher sets GOW3_ASYNC_SHADERS=0 for eboot.bin. This overrides async_shaders in probe.ini.

Still open:
- Hair and cobwebs are bright in dark rooms.
  - Hair is pass #33 (vs 58b25301, ps cc2d0c16; alpha blend). Cobwebs and other transparents are pass #35.
  - Both sample the D16 2048 shadow map 0x11352d0000 (pass #5: a depth-ALWAYS full-screen draw first, then 68 casters) and the light accumulation 0x113f8b0000.
  - Suspect: the sun or directional term is unshadowed for forward-lit objects.
  - The guest image dump now supports D16Unorm.
- Frame drops to 45-57 FPS in the temple.
  - A host sample of the GPU command thread: about 49% is in BufferCache::DownloadMemory → Scheduler::Wait, called via SendCommand from BufferCache::ReadMemory. Guest CPU threads read GPU-modified pages, and every fault waits for the whole GPU.
  - GOW3_FAULT_STATS=1 (page_manager.cpp, launcher default for now) prints the counts, the hottest guest RIPs and the 64 KiB windows every 5 s.

## 14 October 2026 (21:1x): temple frame rate and bright hair/cobwebs, measurements

Frame rate (GOW3_FRAME_STATS):
- The temple room runs a steady 45-49 FPS (21 ms median) with 0 shader compiles.
- The GPU command thread is never idle and is blocked on GPU ticks about 50% of the time.
- The DMA sync of the tiled lighting shader (BufferCache::SynchronizeDmaBuffers before each cs 43b8ee5e dispatch) uploads about 3 GB per 5 s, all CPU-dirty resident memory.
  - New live modes via GOW3_DMA_TRIGGER (analysis/uncharted-user/dma.trigger): "full" (default), "off", "pointers <KiB>" (windows around guest addresses in the stage's user data and flattened SRT).
  - Measured: full 45 FPS, off 48, pointers 48 (about 170-260 MB per 5 s). So the sync is about 3 FPS of the loss. The rest is GPU time.
- Next: GOW3_GPU_PROFILE=1 (now a launcher default) for GPU ms per pass.

Hair and cobwebs:
- The D16 2048 map 0x11352d0000 is cleared to 0 inside a rectangle (a z=-1 TriList quad, DX clip space, z clip off, viewport z 0..1). Every LESS caster fails against 0, which is also what a PS4 does.
- All shadow lookups use image_sample_c_lz with compare LESS. Hair ps cc2d0c16 (hero-default.pak), cobweb 7187fa25 (catacombs-1.pak) and eddie 3d0b0bda each have one.
- Live compare overrides ("cmp never|always|normal|border-black|border-white|edge" in the same trigger file):
  - never: the cobweb and hair go dark.
  - always: walls are sunlit too.
  - normal: walls are dark, but the cobweb and hair are lit.
- So the hair/cobweb lookups land outside the cleared rectangle (border 65535) or beyond the texture edge. The compare-sampler setup (clamp/border) is now logged once per setup.

## 14 October 2026 (22:0x): bright hair/cobwebs - cause found: MUBUF ADDR64 loads were unimplemented

- The D16 2048 sun shadow map 0x11352d0000 is a 4-slice array, one slice per cascade (DB_DEPTH_VIEW slices 0-3).
  - Slice 0 is all 0 in the temple: the roof is pancaked to the near plane, which is correct.
  - Slices 1-3 have content. Dumps now include every array layer (_L<n> files).
- Hair (ps cc2d0c16, hero-default.pak), cobweb (ps 7187fa25, catacombs-1.pak) and eddie (ps 3d0b0bda) look up shadows with image_sample_c_lz (DA=1, compare LESS, clamp to edge).
- Their cascade data (192 bytes per cascade) is read with BUFFER_LOAD_DWORD/X4 in ADDR64 mode:
  - vaddr v[0:1] = s30 + 192*cascade; V# s[64:67].
  - The translator ignored mubuf.addr64 and treated v0 as a buffer offset, so the matrices were garbage and the lookups landed in lit texels.
  - The walls' shader does not use ADDR64, so walls were right.
- Fix (vector_memory.cpp BUFFER_LOAD): for ADDR64 untyped dword loads, address = V#.base + v[vaddr:vaddr+1] + soffset + inst offset, read with ReadConst (the direct memory path; the shader becomes a DMA user).
  - Shader cache versions 10 -> 11.
- Tooling: gpu target `gcn-disasm` (tests/gcn_disasm.cpp) disassembles GCN binaries with the recompiler's decoder. Shader binaries come from the game packs by their OrbShdr footer hash.

Hair/cobweb FIX confirmed (22:1x, user screenshot):
- The cobweb is dark like the room and Nate's hair is natural. No flicker: 6 consecutive screenshots give the same cobweb-region brightness.
- The first ADDR64 build flickered. DMA reads of pages that the BDA page table did not map yet return 0 and are mapped only after the submission (FaultManager), and per-frame data lives on fresh pages.
  - Fix: DMA sync mode "pointers" (launcher default GOW3_DMA_SYNC="pointers 256") calls ObtainBuffer on 256 KiB windows at each guest address in the DMA stage's user data / flattened SRT data. The windows become resident and current before the draw.
  - The full per-draw sync is gone: 490 -> 75 ms per 5 s on the GPU thread.
- Flatten pass: ReadConsts with a per-lane (non user-data) base are left dynamic, and SimplifyReadConstAddressAdd checks the base arity.

Temple frame rate (47-49 FPS), cause:
- About 1000 GPU downloads per 5 s with a full GPU wait each (buffer_cache.cpp DownloadMemory; about 2.5 s per 5 s).
- They are all CPU writes into GPU-written memory in 0x1038d00000 / 0x1038e00000 (GPU constants of cs 2d215c3b), 0x1036200000 and 0x1061e00000.
- They come through Rasterizer::OnWriteFault -> InvalidateMemory, with the download run on the GPU thread (SendCommand).
- Fix in test: BufferCache remembers 1 MiB windows where this happened (IsCpuWriteHot). GPU storage writes up to 4 MiB in those windows join the per-submission batched download (SyncSmallGpuWrites), so the CPU's later writes find clean pages.
- Measuring tools: GOW3_FAULT_STATS (download stats by window), GOW3_PERF_DIAG (wait profile by caller), GOW3_FRAME_STATS, GOW3_GPU_PROFILE (now works for Uncharted: frames are marked at the display pass after a flush).

## 14 October 2026 (22:5x): temple frame rate - lazy CPU writes into GPU-written pages (in test)

Measured before the change (brazier room, 46-52 FPS):
- About 800 CPU write faults per 5 s land in GPU-written pages. Each one downloaded a 512 KiB window with a full GPU wait (buffer_cache.cpp DownloadMemory, about 2.5 s of waiting per 5 s; "GPU ticks" blocked about 50%).
- None of the faulting writes touch GPU-written bytes. They hit other bytes in the same page: about 300 GPU-written bytes per page.
  - 0x1036221bf4..c0c: GDS-to-memory DMA copies.
  - 0x1061e00000: 1 KiB storage writes of cs 6a6422a9.
- The 2d215c3b scalar-copy HLE (vk_shader_hle.cpp) had already removed the 0x1038d/e stalls.

Change (BufferCache::TryLazyWrite and LazyPage; GOW3_LAZY_WRITES=0 turns it off; relaxed readbacks only):
- A write fault whose bytes don't intersect gpu_modified_ranges unprotects the page and skips the download. The guest copy of the page's GPU-written bytes stays stale and is recorded.
- Uploads (SynchronizeMemory, through ForEachLazyUploadPiece) leave out recorded bytes whose guest copy still matches the record, so the GPU values in the arena survive.
- A byte that differs from its record was written by the CPU after the GPU, and the CPU value wins:
  - an upload sends it and subtracts it from gpu_modified_ranges;
  - a download (WriteBackGpuData) doesn't overwrite it.
- Downloads erase the records of their pages.
- The "Download stats" line now adds "lazy writes (no download)" and the number of GPU-written bytes the CPU overwrote later.

## 14 October 2026 (23:1x): async readbacks, 60 FPS in the temple; treasure-pickup crash fixed (in test)

Result of lazy writes:
- The DownloadMemory waits are gone.
- The next limit was the per-submission readback of small GPU writes (SyncSmallGpuWrites -> DownloadRanges, ~10 ms Finish per frame, 44 FPS with ~2600 draws per frame).
- Turning that readback off ("off" trigger) didn't help: the CPU then wrote GPU-written bytes about once per frame (0x1036200000), each a full wait.

Async readbacks (BufferCache::DownloadRangesAsync; GOW3_ASYNC_READBACK=0 waits instead):
- The copies are recorded at submission end into a held HostCached staging buffer.
- PollReadbacks (per BindResources and at OnSubmit) writes them back once the tick passes.
- Until then the pieces stay in gpu_modified_ranges. `rewritten` holds later GPU writes, which stay GPU-modified.
- A CPU write fault on bytes whose newest data is in a readback waits for that tick only.
- Synchronous downloads complete overlapping readbacks first.
- User result: steady 60 FPS (frame median 16.65 ms, ~3000 draws per frame).

Crash on picking up a treasure: STOP in ir_emitter Pack4x8 "Unsupported 4x8 number format: Float".
- cs 8f067540 bound garbage V#s (Clamped size errors).
- Likely cause: the 2d215c3b scalar-copy HLE checked GPU-modified per page. Lazy pages and pending readbacks keep page bits set longer, so copies went to the GPU, and the SRT walker read their output a frame late (uninitialised for a new effect).

Fixes:
- The HLE checks bytes (BufferCache::IsRangeGpuWritten) and unprotects dst itself (PrepareHostWrite, lazy snapshot of other GPU bytes in the page).
- The pack/unpack helpers in ir_emitter.cpp log and fall back to the default format instead of UNREACHABLE.
- GOW3_FAULT_STATS prints "Scalar copy HLE (5 s): N on the CPU, M on the GPU".
- Launcher: GOW3_GPU_PROFILE and GOW3_WATCH_RANGE are off (the profiler splits render passes).

## 14 October 2026 (late): Uncharted launcher, frame-rate unlock, render resolution

- **Launcher:** launcher/uncharted_launcher.pyw (Tkinter + Pillow), started by Start-Uncharted.cmd and Uncharted.lnk.
  - Game cards for U1/U2/U3; quality presets Low (720p/30) / Medium (1080p/60) / High (1440p/60) / Ultra (1440p, unlocked); Display, Performance and Advanced tabs.
  - Writes probe.ini (shared with the in-game menu); launcher-only options go to %APPDATA%/Uncharted Launcher/launcher.json.
  - Env passed to tools/probe_uncharted.py: UNCHARTED_DIAGNOSTICS, GOW3_VBLANK_HZ=0 for limits above 60, UNCHARTED_RESOLUTION, GOW3_DRAW_PIPE / GOW3_VK_RECORD_THREAD (multithreaded rendering, experimental, off).
- **Game runs as gow3_pc/out/Uncharted.exe**, copied from gow3-probe.exe at launch. The exe icon, the window icon (sce_sys/icon0.png) and the title are Uncharted.
- **In-game menu:** the GoW tabs and rows are removed (cheats, orbs, enemy health bar, engine frame rate, resolution patches, game hooks). FPS limits are 30/60/120/144/165/240/unlimited, without the engine-fps cap.
- **Frame-rate unlock:** the present thread uses the menu limit; above 60 needs a 480 Hz vblank (GOW3_VBLANK_HZ=0). Untested in game (game speed).

Render resolution (tools/probe_uncharted.py resolution_patch, UNCHARTED_RESOLUTION=WxH; all three games have the same code):
- The display settings constructors and the 1080p/720p mode switch store the size in many fields:
  - 1920x1080 at +0x08/+0x18/+0x24/+0x38
  - tiles +0x40 = w/8, +0x44 = h/4
  - a larger size at +0x30/+0x34 (2560x1216)
  - packed movabs 0x43800000780
  These are 33 stores in total. There is also a generic SetResolution(w, h, aspect) that computes the same fields.
- Reporting a 4K display (sceVideoOutGetResolutionStatus) has no effect: the size is hard-coded.
- Memory table (32-byte entries {id, parent, u64 size, u64 align, 0}):
  - Render Target Heap: id 0x85, parent 6. 497 MiB in U1/U2, 561 MiB in U3.
  - GPU (garlic) block: id 3, parent 2. 2988 MiB in U1/U2, 3052 MiB in U3.
  - Both grow by heap * (pixel factor * 1.1 - 1), rounded up to 64 MiB. A non-aligned garlic crashed right after the onion mapping.
- The table check `lea rsi,[rbp-0x20]; movabs rdx,0x120000000` expects the direct blocks to total 4608 MiB, so it's raised by the extra too.
- The runtime extends the game's 5056 MiB search end to GOW3_DMEM_MB (runtime_memory.c).
- **Tested** (boot to title, 45-50 s, no asserts): 720p and 1440p in U1; 1440p in U2 and U3.
- **Limits:**
  - The "GPU Mem" heap is the whole garlic block, and its size is cut to 32 bits: at 4K (4716 MiB) it became 620 MiB, and the 368 MiB sub-heap 0x87 failed. Garlic must stay under 4 GiB.
  - At 1800p a second heap, "Render Target" (id 0x84, 139.77 MiB), ran out (a 45 MiB request).
  - 4K needs the 32-bit cut fixed. The heap names are set by static constructors around 0x4123xx (GPU Mem object 0xd93c40), and the size comes from a separate init that hasn't been found yet.
- MediEvil (reference/medievil 3a16e0e) did 4K with the same method: a bigger framebuffer pool plus the address-range checks in its free paths.

## 15 October 2026 (00:xx): U2 crash (fiber stacks), native 4K

**Uncharted 2 in-game crash** (exit 0xC0000005, nothing logged; WER: fault at 0xb8ecf3e = guest 0x4ecf3e)
- Crash site: a `ChooseJob` worker's `mov [rsp+0xc0], rax` hit a write-protected stack page. With the stack unwritable, Windows can't push the exception frame, so it kills the process silently.
- Fiber stacks come from game memory (sceFiber contexts). The DMA pointer windows (256 KiB around any dword pair that looks like an address) can cover them.
- Fix: gow3_fiber.cpp registers every fiber context (Initialize/AttachContext; Finalize unregisters), and AddressSpace::Protect (gow3gpu.cpp Gow3KeepWritable) never write-protects those ranges.
- Tried first and discarded: a no-protect window sync. Re-uploading writable pages cost ~1 GB/s.
- Image layout: the runtime image is the end of boot-linked.bin (size at header offset 8) at base 0xb400000. Guest offsets are image offsets. **boot.bin is NOT the loaded layout.** Helper: scratchpad re/img.py.

**4K**
- Memory areas: entry 2 onion = context 5 ("CPU Mem"), entry 3 garlic = context 6 ("GPU Mem"). MemoryArea (memory-area-ps4.cpp) holds one page id per MiB in a 4096-entry array, so one area can't exceed 4096 MiB. MemoryArea::Initialize asserts MiB-aligned sizes. Each area starts on a 4 GiB boundary.
- Fix: above the garlic limit the Render Target Heap (0x85) moves to the onion area (context 5, rounded to 64 MiB). Garlic's rounding goes to the second RT heap (0x84), which also grows with the pixel factor.
- Boot-tested at 3840x2160 (title screen, 50 s, no asserts): U1 and U2. Gameplay is not tested.
- U3 at 4K, three attempts:
  - First: "HALT: Command context dcb full! Increase size!" (ndgi-orbis.cpp ComputeContextBufferFullCallback). The compute contexts are set up by the function that installs that callback; buffer sizes are "Deform Entities" 16 KiB and, U3 only, count*0x1400+0x1c00 capped at 0xd000.
  - Buffers enlarged x4: corruption crash (wild pointer at guest 0xe5260b).
  - Buffers enlarged x2: corruption crash at 0x55bc10.
  - Likely more buffers sized for 1080p (tiled lighting lists?). U3 is clamped to 1440p in probe_uncharted.py for now.

## 15 October 2026 (00:3x): resolutions above 1080p fail in gameplay; presets back to 1080p

User sessions:
- **U2, 1440p, 60 FPS:** played for minutes, then the game assert `m_lookAt.X() == m_lookAt.X()` (NaN) in UpdateFocus, player-base.cpp:1101.
- **U1, 4K Ultra:** assert `offsetTableIndex != -1 && offsetTableIndex < GenericOffsetTable::s_totalValidOffsetTables` (BP/Shared/TGenericOffsetPtr.h:100): Bluepoint's offset pointer tables don't cover the Render Target Heap after it moves to the onion area. Just before: 32k guest read faults per 5 s at 0x10ecxxxxxx (that heap).
- **U3, 1440p:** corrupted pointer while loading into the game (guest 0xe5dca2, same area as the 4K corruption).
- The green cutscene at 1440p is probably the same class of problem.

Changes:
- The launcher presets all render at 1080p. High and Ultra use FSR 1 to the screen; Ultra unlocks FPS with 2 frames queued.
- Resolution above 1080p is labeled experimental. Config version 2 resets a saved resolution to 1080p.
- U3 is clamped to 1080p in probe_uncharted.py.

Next for >1080p:
- CPU readbacks of render targets that assume the 1080p layout (look-at NaN).
- Buffers sized for 1080p (U3).
- BP offset tables, for the onion move.

## 15 October 2026 (01:00): U3 crash cause, U3 hair shadow flicker

**U3 "dcb full" crash at the London pub, even at 1080p**
- Bisect with the user: all three new mechanisms off: OK. Only GOW3_ASYNC_READBACK off: OK. So the cause is the asynchronous readbacks. U3 runs with GOW3_ASYNC_READBACK=0 (probe_uncharted.py).
- Planned fix: read-protect pages with in-flight readback pieces; a guest read waits for that readback only.

**U3 hair lit for a frame now and then (sunlit museum)**
- Frame dumps: about 1 in 8 frames loses the window-frame sun shadow on hair. The ADDR64 hair shaders (ps 5d1bfb2d / 9e3f2c9d / 47f0eee5 / b107ecc9, pass 29) read cascade data at (s42:s43) + v2.
- Ruled out: DMA page faults (0, new GOW3_FAULT_STATS "DMA page faults" line), full DMA sync, compiles and skipped draws.
- Cause: DMA reads are invisible to the barrier tracker. The window uploads recorded in SyncDmaMemory (end of BindResources) never set needs_barrier, so a read could see a copy mid-flight, and later copies into the arena had no barrier against the draw's reads.
- Fix: needs_barrier |= IsBufferAccessed(arena, whole) for the window arenas, and ResetBindings registers a shader read of those arenas (dma_read_arenas). Built 00:59; the user's test is pending.
- The U3 hair shaders were extracted from the actor33 packs (young-drake.pak etc.; scratchpad find_shaders.py and u3shaders/).

## 15 October 2026 (night): update 1.02, resolution causes, movies above 1080p, plan

Plan and test list: analysis/PLAN.md.

- **Update 1.02** (`utndc.v1.02.pkg` → `game/CUSA02344-patch`, metadata from the PKG entries): `tools/probe_uncharted.py` finds `<game folder>-patch|-update` (or UNCHARTED_UPDATE) and runs a linked view (gow3_pc/scripts/game_update.py; fixed a stale link record that kept the view from being removed). All three executables were recompiled. Patches are hex patterns with `??` wildcards and per-version variants.
  - GPU wait: in 1.02 the 2.0 s timeout jumps straight to `int 0x41` (U1 0x4fdf96). Same bytes in U2/U3 (`c5f258c0 c5f82e05 ???????? 7710 f390 498b00`).
  - Resolution: the 1.02 constructors no longer store 1920/1080; only the display settings init (U1 0x4d0710, global 0xe76e74: +0x08/+0x18/+0x24/+0x38 sizes, +0x68 60.0f) does, with [rax]. Total check uses lea rsi,[rbp-0x30] in U2/U3.
- **Frame timing**: dt = rdtsc delta / TSC frequency (U1 1.02 0x9d419), clamped by "Minimum Framerate" (clock update 0xfac70); `forcefps <n>` overrides. Game speed is right above 60.
- **>1080p crash causes**:
  - U2 1440p NaN and U3 1440p pointer crash: async readbacks were on (584/230 in flight).
  - U1 4K "offsetTableIndex != -1": GenerateOffset (U1 1.02 0x662ae0) packs pointers as table<<31 | offset; tables fixed at [0, 2 GiB) and [0x1000000000, +2 GiB) (set at 0x7e156). The moved Render Target Heap is created before the level heaps (0x82, 0x80), so they land past 2 GiB (failed pointer 0x10e92cbd1c, animation loader). 4K frame capture: RTs at CPU area +1008..2393 MiB. Neither 1 MiB alignment nor context 8 (null heap at boot) moves it. U1 renders at most 2880x1620 (move=False) and upscales.
  - The 13 Oct 1080p assert of the same kind: pointer 0x8007c301a, a rodata string, with the image at 0x800000000 (before --low-image). Table 0 is needed.
  - U3 1.02 at 4K: CPU area mapped 100 MiB larger than the table (4132 MiB wrapped to 36 MiB). 128 MiB headroom; margin falls back 1.1 → 1.05 → 1.0.
  - Boot-tested: U1 1440p/1620p, U2 4K, U3 4K.
- **Movies above 1080p** (green, tiled): the movie player (movie-rendering.cpp, U1 1.02 0x4fb7b1) builds the luma/chroma textures with the display size (+0x08/+0x0c) and the chroma offset W*align(H,64) from +0x24/+0x28; frame buffers are fixed at kSizeFrameBuffer = 4 MiB (movie-player.cpp). Now: gpu/shim/gow3_movie.cpp + videodec2 write a copy scaled to GOW3_MOVIE_SIZE into flexible memory at 0xf000000000 (runtime_map_guest_memory) and ImageInfo reads the planes there. The conversion shader (cs c85d9816) samples at (x+0.5)/1920, (y+0.5)/1088 as literals: shaders holding both literals get them rescaled in an IR step (recompiler.cpp) and a cache key with the display size (regs_shader.h GetParams, Gow3Movie::ShaderKey). Rows past the frame height repeat the last one (1620p). Verified at 1440p and 1620p: same picture as 1080p.
- **Low-end reference**: integrated Radeon (GOW3_GPU_ID=1), U1 title: 21 FPS at 1080p, 44 at 720p.
- **Red zone** (shadPS4 #5038): Windows exception frames overwrite the SysV red zone. About 340 red-zone blocks per game, none at known crash sites.
- **Safe async readbacks** (GOW3_ASYNC_READBACK=1; =2 the old unprotected mode): PendingReadback.watched = merged pages of its pieces, read-protected (PageManager read watchers) from the first CompleteReadbacks after its submission (protecting at record time let the GPU thread fault on its own unsubmitted tick); a read fault on such a page completes readbacks up to that tick and returns (relaxed). Fiber stacks now keep read access too (AddressSpace::Protect). U2/U3 boot with it (U2: ~900 readbacks / 5 s, ~10 reads waited). U1 exits 0xC0000005 while loading right after 0x119de2e000 +0x61000 is protected, with no access violation reaching the vectored handler (GOW3_FAULT_TRACE=1 shows the first 64): probably a stack in that range. Launcher option per game, forced off for U1.
- **Display modes** (window.cpp): the window was created at 1920x1080, so on a 1920x1080 screen "Windowed" put the title bar off screen over the taskbar; exclusive "Fullscreen" minimized itself whenever the game window lacked the focus (SDL's default), which it often did when started from the launcher. Now: windowed is 16:9 within 85% of the usable desktop, centered, and restored when leaving fullscreen; SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0; SDL_RaiseWindow on each mode change; the launcher calls AllowSetForegroundWindow(ASFW_ANY) before starting the game. Verified (GetWindowRect): windowed 1559x876 centered with caption, borderless and fullscreen 1920x1080 in the foreground. UNCHARTED_CONFIG_FILE points tools/probe_uncharted.py at a test settings file.
- **U1 card started U2** (user, 15 Oct): the U1 card started eboot.bin without arguments, i.e. the Collection's game selection, whose cursor sits on the last game played; one Cross started that game (LoadExec big2). eboot.bin's argument parser (U1 1.02 0x5cf1a9) knows "ispartofuccollection" (sets +0x39c1 and clears +0x39b0, as the Collection starts a game): with "eboot.bin ispartofuccollection" it opens Drake's Fortune's own title. That is now the default (probe_uncharted.py --collection-menu for the selection). Also: typing in another window while the game window took the focus pressed buttons (Space = Cross); GOW3_KEYBOARD=0 turns the keyboard input off for test runs (default on).
- **Keyboard and mouse** (15 Oct): gpu/shim/gow3_mouse.cpp captures the mouse (SDL relative mode) while the game window has the focus and no menu or text dialog is open, and hands movement and buttons to runtime_pad.c (gow3gpu_mouse_take). Mouse speed becomes the right stick (smoothed ~25 ms, dead-zone offset 0.15, GOW3_MOUSE_SENS, GOW3_MOUSE_INVERT); mouse buttons and the wheel map to PS4 buttons (GOW3_MOUSE_MAP), keys to buttons and stick directions (GOW3_KEY_MAP: ls_*/rs_* names; the first entry of a button replaces its defaults, more entries add, an empty name unbinds). New PC defaults: WASD, mouse look, RMB L2, LMB R2, Space Cross, LCtrl Circle, F Square, R Triangle, Q/X1 L1, E/X2 R1, LShift L3, V/MMB R3, Esc/Enter Options, 1-4 d-pad, wheel d-pad right/left. Launcher CONTROLS tab per game. Launcher: YouTube/GitHub links, own AppUserModelID (taskbar icon).
- **DLSS** (15 Oct): DLSS 5 (neural rendering) has no public SDK. NVIDIA/DLSS on GitHub is at 310.9.1 (8 Sep 2026; the NGX header keeps feature 18 as Reserved18) and Streamline at 2.14.1. The community DLSS 5 tools use leaked or driver-store nvngx_dlssnr.dll through a shim that bypasses NVIDIA's caller check (0xBAD00002), only on D3D12, and they are not redistributable, so they are not used here. The possible path is DLSS 4.5 Super Resolution/DLAA through the existing bridge (vk_dlss.cpp, gpu/dlss_bridge, packaging/windows/build_dlss.sh, official SDK). It needs per-game hooks. In capture frame_20261014-104215_2 (1080p), the scene HDR color is RGBA16F 0x1133ab0000, written by a 716-draw pass with scene depth D32S8 0x1130438000. The post chain then runs: 960x540 RG16F motion/velocity-like targets, 480x270 bloom, R8 1080p, and RGBA8 1080p before the sRGB display buffer. Camera constants and jitter are still to be found.

## 15 October 2026 (morning): U2 treasure crashes, readbacks, torch

- **Torch/flashlight light nothing in U2 (FIXED, user-confirmed 09:4x).** Same cause as U1 on 14 Oct: the light ramp LUT (256x8) is drawn once at load and was skipped while its pipeline compiled in the background. Fixed generally in PipelineCache::GetGraphicsPipeline: draws whose color targets are all <= 256x256 pixels compile at once (sync_compile; a pipeline already queued is waited for). U1 still runs fully synchronous (GOW3_ASYNC_SHADERS=0).
- **SRT tables written by the GPU earlier in the same command stream** (Rasterizer::SyncSrtTables, hook Shader::g_srt_pre_walk in Info::RefreshFlatBuf; GOW3_SRT_SYNC=0 off): the walker's tables are decoded from its code (push rdi / mov rdi,[rdi+off] / reads / pop rdi), and tables with GPU-written bytes are downloaded before the walk. cs 7d7e7206 / a0b87e41 tables (12/240 bytes in per-frame rings 0x1100?da2c0) are GPU-written: ~15 syncs/s at the title, ~1 per 5 s in play.
- **Treasure effect garbage V#s (cs 7d7e7206, 8f067540):** the V# slots at table offsets 0x80-0xb0 hold floats for the treasure effect (CPU-written, not stale). Bound, written ones made huge ranges "GPU-modified" (game data overwritten by stale GPU copies: fg-draw-mgr `m_gfxEopTick` and mesh.h `iSubMesh < m_numSubMeshInstances` asserts) and a 3.8 GB arena ran out of device memory. Now a V# reaching past mapped memory (not num_records ~0) or spanning >= 1 GiB is bound as null ("Ignoring a garbage V#"). Only these two shaders ever had clamped sizes. DescribeSharpSource prints where a garbage V#/T# came from (table, offset, GPU-written state).
- **Async (safe) readbacks:** heavy U2 area 53-60 FPS (sync: 44). Fixed a recursion: WriteBackGpuData's lazy-page snapshot read a page this readback still watched; the fault completed the same readback again until the stack overflowed (silent exit in ___chkstk_ms). Lazy snapshots now read through the backing view (runtime_memory_read_backing / MemoryManager::TryReadBacking). Still open: with async on, both asserts above happened (before the garbage-V# nulling); retest pending.
- Per-frame sync off ("off" trigger) only moved the wait: three 1 MiB triple-buffered windows (0x1102600000/0x1102b00000/0x1103000000) get CPU writes into GPU-written bytes each frame (~10 ms waits each); 47 -> 50 FPS.
- Launcher writes the start script's output to analysis/uncharted-user/launcher-last-run.log.
- **U2 torch "points with the camera" (15 Oct): original behaviour, not a bug.** Live light records (image 0x1157760, 0x60 each: pos, dir, color, range/cone) and the frame capture agree: the player flashlight light (record 0, Flashlight process vtable image+0x100bca0, Active state update 0x69a190, attach locator via attach-system GetLocator 0x3b4620) sits at Nate's torch and its direction equals the camera's z axis exactly (ps a24da0bb slot 2 vs slot 0). The game aims the torch with the camera (script FlashlightFaceForward overrides it per scene). Precise readbacks changed nothing. Earlier "light origin at the camera" came from misreading the camera view-projection (vs 1bf62085) as the light matrix. Tool: scratchpad re/peek.py, re/flscan.py (ReadProcessMemory, read-only).

## 15 October 2026 (12:4x): frame-rate cap 120, launcher and in-game menu settings in step

- U1 at fps_limit=165 (140-160 FPS, 480 Hz vblank): the user found Nate falling outside the level (flat beige out-of-bounds geometry). The 120 cap of EmulatorSettings::GetFrameLimit never applied to Uncharted (the menu limit path used PresentLimit, which passed 165 through).
- Now: FpsLimits = 30/60/120 (gow3_graphics.h MaxFpsLimit); ParseFpsLimit maps 0 and >120 to 120, others to 60; PresentLimit caps at 120; default fps_limit 60. The launcher offers 30/60/120 only and Ultra uses 120 (it used the monitor rate, 165 here).
- Launcher showed 165 after choosing 60: the launcher works alone (sandbox click test), but (1) Ultra set the monitor rate and (2) the in-game menu's Save wrote the game's whole in-memory settings (165) over the launcher's change. Save now merges: keys unchanged in game since the last Load/Save keep the file's value (unit test: scratch settings_merge_test.cpp). The launcher re-reads the file when it changes (about once a second, and before Play) instead of writing its older values back.

## 15 October 2026 (13:xx-14:1x): U1 bullet impacts/blood and movie colours above 1080p FIXED (user-confirmed)

- **Bullet impacts, bullet holes, blood:** U1 1.02 casts bullet/decal rays on the GPU (meshraycaster.cpp: Cs_RayTrace 0x308607aa, Cs_RayTraceGatherHits 0xc92150ba). ProcessRayCasts (0x5460a0) records the batch with an EVENT_WRITE_EOP (data 1, interrupt) into the frame's GPU done counter (m_pGpuDoneCounter = the render frame's m_gfxEopTick); UpdateMeshRayCaster (0x547340) calls HandleResults (0x5475a0) when [counter+0x20]==0 and the decal callback OnRayCastResultsReady (decal.cpp) places holes/blood. Our EOPs are written when recorded, so the game read the hits before the GPU made them (every shot a miss). Fix: GatherHits' written buffers are collected (Rasterizer::eop_readbacks) and at the next EOP read back asynchronously with their pages read-protected (BufferCache::DownloadRangesAsync protect=true; a guest read before completion waits for that readback only). Tried first: synchronous download at the EOP (works, ~10 ms GPU wait per frame, 47 -> 29 FPS at 4K) and delaying the EOP until the readback (crash: assert GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick in FrameBegin, fg-draw-mgr.cpp:234). Not the cause: the `is-japan?` check (0xfe0e0; settings+0x3994 region from a table, 0 = Japan) and the debug flags Enable/Render Decals (0x955a18/0x955a17), Show Blood Effects - all on.
- **Movie colours above 1080p** (grey picture, diagonal coloured lines, U1 at 1620p): GPU texture dumps (vk_presenter now dumps R8/R8G8 images too) against guest memory showed luma identical and chroma rows shifted 64 bytes per row: the chroma plane (1440 R8G8 texels at 2880 wide) is a linear-aligned texture whose rows the GPU reads at a pitch padded to 64 texels (1472, mips_layout pitch), while the scaled copy wrote 2880-byte rows. Gow3Movie::ScaledChromaPitch() = Align64(W/2)*2 bytes (2944 at 2880; unchanged at 1440p and 4K, where the earlier check passed). The decoder also writes each scaled frame through the backing view in one piece and invalidates it explicitly (no per-row faults).
- Frame capture + window capture method: capture.trigger (pass count) lists sampled images per pass; dumps/guest-images.trigger + dumps/trigger dumps them; scratch winshot_game.py / peek.py read the running game without input.

## 15 October 2026 (14:xx): U2 frame rate falling over minutes FIXED (user-tested)

- Symptom: in a static heavy U2 scene (about 3,400 draws per frame) FPS fell steadily, 42 -> 31 in 3-4 minutes, and process memory grew about 1 MB/s; CPU not throttling (9950X3D at 128% of base), renderer container sizes flat (new "Sizes:" lines every 5 s).
- Found with a new sampling profiler (gpu/shim/gow3_profiler.cpp, GOW3_PROFILE=<seconds>: suspends the GPU command thread about once per ms, writes module+offset counts to GOW3_DUMP_DIR/profile_<n>.txt; symbolize with llvm-symbolizer, image base 0x140000000; scratch symprof.py / cmpprof.py). BufferCache::ObtainBuffer grew from 7% to 26% of the thread, all of it in the page_writes prune (buffer_cache.cpp, from the shared-pages work this morning): a page's list grew by one entry per frame because entries were only dropped once their bytes stopped being GPU data, and every binding of a written buffer scanned the whole list with an interval-set check per entry.
- Fix: a write of a range already listed updates its tick; the prune drops entries GpuBytesInFlight ignores (finished, older than the last readback); a list still at 16 keeps its 8 newest. After: FPS flat at 40-41 for 4 minutes, ObtainBuffer 1.7% and flat, 3,152 entries constant. Process memory still rises about 0.8 MB/s (no effect on speed seen; to look at later).
- Also from this session: the U1 GPU ray-cast readback also runs in U2 (same shader c92150ba, 1 KiB ranges, about 40 per second); fence labels written through the backing view are now excluded from pending readbacks (BufferCache::NoteCpuWrite) after one U2 crash with the FrameBegin m_gfxEopTick assert (no label was later seen inside a readback; the crash did not recur in three sessions). Compute ReleaseMem in order: compute drains about 1,000/s, down from about 34,000/s.
- **U2 Nate invisible / floating (15:xx) FIXED:** bisected with test-env switches (ray-cast readback, torch patches, SRT sync, async readbacks ruled out): the scalar-copy HLE of the 64-wide copy shader 6a6422a9 (added this morning) made Nate float above the ground or vanish (camera inside him). Its shader semantics are right (dst[i] = src[i], i = group*64 + thread < s6); the CPU copy runs when the command is processed, earlier than the console's GPU, and the data it copies (the player's) is completed later. Now 6a6422a9 runs on the GPU by default (GOW3_SCALAR_COPY64_CPU=1 restores the CPU copy); 2d215c3b keeps its CPU copy (since 14 Oct). A per-frame "source changed after the copy" check was tried and removed (it flagged 282 per-object blocks and did not fix Nate). GOW3_SCALAR_COPY_GPU=1 still keeps both on the GPU; GOW3_EOP_READBACK=0/2 switches the ray-cast readback off / to synchronous.
- **U2 crash with multithreaded rendering (assert GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick in FrameBegin, fg-draw-mgr.cpp:234; 14:17 and 15:48, both with the draw pipe on; none in about ten sessions with it off):** FrameBegin (U2 1.02 0xbd4260) takes frame N-3's params slot (0x1588630 + (N & 31) * 0xa70) and asserts its m_gfxEopTick (+0xa28) is set; PrepareNewRenderFrame (0xb62e00) zeroes it and s_lastPreparedFrameNumber is 0x10508d0. m_gfxEopTick is the GPU clock an end-of-pipe event writes. With the draw pipe that event is deferred to the recording thread (SignalAfterHostCopies); WriteData, EOS and compute releases wait for deferred signals first, but RunDmaData did not, and its CPU paths (memcpy, and since this morning WriteThrough for fills/copies into GPU-written pages) wrote guest memory at once, so a DMA-written frame marker could overtake the timestamp. Fix: RunDmaData into memory waits for host copies and deferred signals first (in test).
- **MT crash fix held** (11+ minutes with the draw pipe, 15:53 session). **MT frame time:** the wait profile showed buffer_cache.cpp:846 (CompleteReadbacks wait) about 1.5 s per 5 s: every ray-cast readback (about 36/s) had a guest write into its 1 KiB range while pending ("CPU writes that waited for one" = readbacks), about 8 ms each, a third of the frame. Only with the draw pipe (non-MT sessions: 0 waits): the recording thread runs behind, so the guest refills the result buffer for its next batch before the readback is submitted. Fix: EOP readbacks are yield_to_cpu: a CPU write into a piece adds it to `rewritten` and drops it from gpu_modified_ranges (no wait, no download); counted as "ray-cast pieces dropped for CPU writes" (in test).
- Live toggle A/B in MT (cost per draw on the recording thread): PreparedResources/DrawPreparation (4096/8192) 3.3 -> 3.2 us/draw, at most +1 FPS; DeferredStreamCopies/ParallelCopies (16384/131072) about +1-2 FPS in non-MT (noise). The default mask 159760 stays.
- **Frames queued 1 -> 2 (U2, 17:4x):** the draw recording thread spent 41% in ZwWaitForMultipleObjects and vk_presenter.cpp:609 waited 2.2 s per 5 s (CPU and GPU took turns; GPU 50-70%). With 2 frames queued the present waits went to 0 and the heavy spot rose from 33-37 to 42-50 FPS (MT), but the FrameBegin m_gfxEopTick assert came back after about 6 minutes (MT + 2 queued, 17:48, log u2-mt-q2-crash-1748). MT + 1 queued ran 24 minutes after the DMA fix. With threaded Vulkan recording off (mask bit 16, the default) EOP signals are not deferred to the copy threads, and the EOP flip runs in order on the recording thread (SignalFlip waits for deferred signals), so the remaining MT race is not yet understood. Next: non-MT + 2 queued (no crash seen without MT).


## 16 October 2026: readbacks, ray-cast fences, floating NPCs (0.1.2)

- **U3 Cartagena crash / flying characters:** async readbacks unprotected (GOW3_ASYNC_READBACK unset =
  2) let late readbacks overwrite fresh guest data (0xffff... pointers, compute ring with a garbage
  IB). U2/U3 now use mode 1 (probe_uncharted.py). Mode 1 hung U3 loading: the GPU thread faulted on a
  read-protected page inside MemoryTracker::ForEachUploadRange (region spinlock held) and the handler
  took the same lock -> uploads copy through the backing view (runtime_memory_copy_backing).
- **Mode 1 cost:** 89% of read faults were false sharing (BufferCache::noisy_pages: unprotect for
  10 s), the rest the 16-byte luminance (cs 7095d6c2/4f81a8f) read every frame -> recurring pieces
  (same address and size landed < 500 ms ago) stay readable. U2 33-46 -> 60 FPS with mode 1.
- **U2 NPCs from the sky:** EOP labels written at record time; the guest read ray-cast results
  (cs c92150ba) before the GPU ran them, and the 15 Oct drop of pending ray-cast pieces on CPU
  writes discarded ~250 of ~250 per 5 s. Now an EOP that carries ray-cast readbacks writes its
  label/interrupt when the readback lands (BufferCache::DeferUntilLastReadback); while one is
  pending all later EOPs queue behind it (strict order: a later fence first let U2 overwrite a DCB
  the GPU thread was reading, "Unimplemented PM4 type 0"); the command buffer is flushed at once;
  readbacks are polled every 64 packets and while idle; WAIT_REG_MEM counts a deferred value as
  written (landing it instead cost 18 FPS). Without the flush the readback was never submitted ->
  FrameBegin m_gfxEopTick assert (the ray-cast EOP is the frame EOP in U2). Fences land in 2-9 ms.
- **Floating Nate/NPCs (U2), confirmed by the user in play:** 2 frames queued, DeferredStreamCopies
  (toggle 16384) and a hot-window cooldown each brought it back (they delay or drop GPU data the
  game reads, or copy guest data later). Defaults: 1 frame queued (launcher config version 6),
  mask 159760, no cooldown (GOW3_HOT_WINDOW_TTL=<s> for tests).
- **Performance:** runtime_memory_copy/read/write_backing use the per-thread region cache (the
  winpthreads rwlock per call was 10-15% of the GPU thread): heavy U2 7.9 -> 6.4 us/draw. GPU
  checkpoints only with UNCHARTED_GPU_CHECKPOINTS=1 (~2%). Profiler call stacks (GOW3_PROFILE,
  profile_*_stacks.txt). Remaining in heavy U2 (3,000 draws, ~50 FPS): driver ~19%, per-draw stream
  copies ~9% (must stay inline, see above), image allocation churn ~2.6%, readback volume ~2 GB/5 s.

## 16 October 2026 (night): U2 floating NPCs / sinking Nate / leg flicker - real cause and fix

- The earlier settings "fixes" (frames queued 1, mask 159760, hot-window cooldown) were
  coincidences: 0.1.1 rebuilt from its tag floats too, and so does another port (Supermedo #33).
- Ruled out by user tests: MXCSR/x87 defaults (FTZ+DAZ 0x9fc0 / 0x037f on every guest thread),
  exact synchronous ray casts (GOW3_EOP_READBACK=2), both scalar-copy HLEs on the GPU
  (GOW3_SCALAR_COPY_GPU=all), the old upload path, GDS-to-memory counter copies (a per-frame
  statistic in a 7-slot ring at 0x1037521bf4).
- Precise readbacks (GOW3_READBACKS=2) fixed it (3 user checks) at ~50 FPS: the guest polls fence
  labels (guest code 0xc1e20a / 0xc1e5bd) on a page that also holds GPU data, 60k read faults/s.
- GOW3_FAULT_STATS now lists, per 1 MiB window, the CPU reads that hit GPU-written bytes (with
  addresses) and the shaders that wrote the window. In precise mode the guest read only the
  luminance value and one value (e.g. 0x1062820014) in 1 KiB blocks written every frame by copy
  shader 6a6422a9. Read back with the submission they counted as recurring pieces (readable, a
  frame old).
- Fix (GOW3_COPY64_EOP_READBACK=1, set for Uncharted 2 in probe_uncharted.py): 6a6422a9 outputs
  join the end-of-pipe readbacks (Rasterizer::eop_readbacks; the fence waits for them,
  RunEventWriteEop). Only the dwords the dispatch writes (min(groups * 64, s6, num_records)) and
  only copies up to 64 KiB: its two large copies per frame (~0.2 and ~2.4 MB) were +300 MB/s of
  readback and are never read. User: NPCs and Nate on the floor, no leg flicker (the flicker came
  back in a run with the fix off), 60 FPS. Readback volume ~2.95 GB/5 s, as before the fix.
- Next for low-end PCs: the pre-existing ~600 MB/s of readbacks (hot windows).


## 16 October 2026 (afternoon/evening): MT fixes, readback cut, optimizations on, U2 aim focus, GPU hang (0.1.2)

- **Multithreaded rendering:** U3 m_gfxEopTick assert = flips queued on the GPU command thread
  overtook the end-of-pipe fences the recording thread writes later (driver.cpp QueueFlipInOrder
  through RunInOrder); then a crash in SyncSrtTables = boost::icl RangeSet gpu_modified_ranges read by
  stage A while stage B changed it (shared_mutex, GpuRangesAdd/Subtract). U3 MT 8 min clean. U2
  floating came back with MT: the stage-B fence path skipped DeferUntilLastReadback; fences with
  readbacks now take the waiting path there too, flips likewise; stage A drains before touching
  readback state; deferred_signals atomic (user-confirmed).
- **Readback volume** (GOW3_READBACK_STATS=1): nearly all was 1 MiB windows of the effect shaders
  (cs 8f067540, 7d7e7206, 0c936dd6), which the CPU never reads (precise-readback statistics). Hot
  windows cool after 3 s (GOW3_HOT_WINDOW_TTL), small writes read back in 256-byte granules
  (GOW3_SMALL_READBACK_ALIGN): U2 2.9 -> 0.46 GB/5 s, U3 3.1 -> 0.48, U1 0.63 -> 0.16 (user-confirmed U2).
- **Disabled-optimizations default 159760 -> 0** (tools/defaults): live A/B at one spot, heaviest U2
  view 43.8 -> 49.0 FPS; U3 GPU command thread idle 22 -> 29%. Played for hours by the user.
- **U2 aim blur:** the aiming depth of field (debug "Enable Aiming Dof", on) reads its focus distance
  from a 1x1 R32F render target (guest code 0x44e2cb reads *(u32*)ptr << 8): never copied back, 0.
  Chain: ps 9f801363 (32x32 crop of the depth at 8 CPU constants) -> 0a15b09b (16x16) -> 784b6f74
  (1x1 min). TextureCache::ProcessTinyReadbacks copies 1-row images into the buffer cache and the
  range comes back with the next EOP fence, unprotected (GOW3_TINY_IMAGE_READBACK=1, U2 only).
  First versions crashed (m_gfxEopTick): protected pages made game threads wait for the GPU; then
  whole pitch rows of staging garbage; then a landing after the fence onto reused memory. Above
  1080p the crop centre c[0],c[1] = HUD crosshair (1920x1080 space) / render size: 0.25 at 4K;
  FixAimFocusCentre divides by 1920*c[6], 1080*c[7] (GOW3_AIM_FOCUS_FIX=0 off). 4K/1440p/1080p
  verified (GOW3_TRACE_PS dumps render targets, viewport, textures and constants).
- **U2 GPU hang after 90 min** (game: "Device lost during submit" after "GPU wait: 1 seconds"; OBS:
  DXGI_ERROR_DEVICE_HUNG the same second): cs 8f067540 bound a garbage V# in mapped memory
  (unaligned, stride ~15 KB, ~1e9 records). For 8f067540/7d7e7206 an unaligned base, stride >= 8192
  or size > 64 MiB is garbage (real bindings <= 33 KB, aligned).
- **Memory/thread growth with OBS:** threads starting in nvwgf2umx.dll (NVIDIA D3D driver) grew ~6/s
  (~90 MB/s) only with OBS's implicit Vulkan layer (obs-vulkan64.json; DISABLE_VULKAN_OBS_CAPTURE=1
  stops it) while OBS kept re-hooking ("failed to open shared handle"); NVIDIA presents Vulkan
  through DXGI/D3D12 here, so OBS hooks both. Not a port leak; README/release notes give the
  NVIDIA "Prefer native" present method or Window Capture.
- Profiler: RtlLookupFunctionEntry deadlocked with the target suspended and RtlVirtualUnwind read
  garbage frames; now its own .pdata lookup and unwinder with checked reads.
- Frame rate limits 40-55 (overlay and launcher) for the U3 cloth flicker tests (60 FPS only).
- GitHub: #1 (U3 crash) and #4 (U1 crash after the boat scene, v0.1.1; user-tested fixed) answered
  and closed.


## 17 October 2026 (night): U3 Sully cloth flicker FIXED (my tests, not yet user-confirmed)

- **Cause:** cloth solver cs 1c957b01 (1024-thread groups, 2 KiB LDS) does warp-synchronous LDS
  reductions (no s_barrier between steps inside one 64-lane wave). shadPS4 only inserts LDS barriers
  for groups of exactly 64 threads; on 32-lane subgroups (NVIDIA) the wave's halves race. The game
  picks the cloth sub-step count from dt (cb#1 dword 160: 2 at 40-60 FPS, 3 at 30): with 2, every
  dispatch with sane input exploded (alternating frames). Proof: AMD iGPU (GOW3_GPU_ID=1, wave64,
  GOW3_TIME_SCALE=3 to get 2 sub-steps at 15 FPS) never exploded.
- **Fix (shared_memory_barrier_pass.cpp):** MultiwaveLdsBarriers(hash) (default 1c957b01;
  GOW3_LDS_BARRIERS_MULTIWAVE=0/1/hex list) runs the barrier insertion for multi-wave groups on
  32-lane GPUs with a stricter uniformity test (IsGroupNonUniform), skips shaders with an early
  Return, and only puts merge-block barriers after ifs/loops that access LDS. Without that last
  rule NVIDIA hung (m_gfxEopTick) on a barrier in a loop without LDS ops that the dispatches never
  entered (found by bisecting with GOW3_LDS_BARRIER_SITES). Cached SPIR-V of listed shaders is
  named <hash>_<perm>_lds1, so old caches recompile only them.
- **Result:** 0 of ~1200 dispatches exploded (was ~50%), 0 flicker frames in 40 shots, 60 FPS, with
  the user's shader cache. GOW3_LDS_BARRIERS_MULTIWAVE=1 (all 12 U3 LDS shaders, 84 sites) also ran
  clean, not adopted (U1/U2 untested). U2 60 s check unchanged (60 FPS, no errors).
- Diagnostics added: GOW3_LOG_LDS, GOW3_LDS_BARRIER_SITES, GOW3_CLOTH_PEEK, GOW3_TIME_SCALE (all
  guest clocks), GOW3_DUMP_CS_CONST.

## 17 October 2026 (night, 02:30-04:40): low-end PCs

- **Weak-PC simulation:** AMD iGPU (GOW3_GPU_ID=1, about a quarter of a PS4 GPU), 720p, game
  process affinity 0x55 (4 cores). Per-thread CPU with names: runtime now sets Windows thread
  descriptions for guest threads; scratchpad threadcpu.ps1 (OpenThread 0x40 + GetThreadDescription).
- **Found:** the six NdJobWorkerThreads spin with `pause` when idle (U3 worker loop 0xda6470): the
  job manager's sleep flag (byte +0x59, SetSleepMode 0xd96440, set on by the game once, off in four
  places) is off. 4 cores: 390% of 400% CPU, GPU thread 5%; U3 title 19-27 FPS on the RTX 5080,
  level 5-7 FPS on the iGPU.
- **Fix:** tools/probe_uncharted.py JOB_SLEEP_PATCHES (3 pattern patches, same bytes in U1/U2/U3
  1.02): the worker loop, its re-check before waiting and the submit path treat the flag as on, so
  an idle worker waits on the manager's condvar after 1000 spins (10 ms timeout) and every submit
  broadcasts. UNCHARTED_JOB_SLEEP=1/0, default auto = laptops (GetSystemPowerStatus battery) or
  <= 12 logical CPUs; launcher Performance -> Idle game threads (Auto/Sleep/Spin).
  UNCHARTED_JOB_SLEEP_SPINS=<n> (diagnostics) changes the 1000.
- **Results:** 4 cores: U3/U1 title and level 60 FPS, 1-1.5 cores used; iGPU + 4 cores U3 level
  27.5 FPS (5-7 before; now GPU-bound, the iGPU is ~4x slower than a PS4). 32 threads, Sleep: U2
  heavy spot uncapped 76.5 -> 78.2 FPS. MT rendering on 4 fast cores: 72-75 off vs 69-72 on.
- **Garbage resource tables:** 1 of 11 four-core Sleep runs stopped with "Thread ID buffer
  addressing is not supported outside of compute" after SRT walker faults at -1 pointers; also
  seen with GOW3_FINISH_AT_FLIP=1 (reproduces it about every second run: VS d5cdb3af walked a
  table of -1 pointers, a draw of 38d69960 compiled from a garbage V#) and GOW3_SYNC_GPU_CONSTANTS=0.
  The GPU thread's own walk reads it, so the table changed before that thread got there; the game's
  frame recycling (FrameBegin N-3, PrepareNewRenderFrame N-32 m_gfxEopTick checks) is frame-based
  and fine. Root cause open. Now tolerated: resource_patching_pass ignores add_tid outside compute
  (logs the shader); the walker fault handler skips the faulting load (register 0) the first 16
  times per instruction instead of patching it to 0 for good (logs "SRT walk fault: shader ...
  user data ..."). With FINISH_AT_FLIP the game kept running at 57 FPS through them.
- **GPU thread:** Rasterizer::IsMapped keeps 4 per-thread mapped intervals (generation bumped on
  unmap): SyncSrtTables 12.1% -> 6.5% of the GPU thread, idle 14.8% -> 19.9% (U3 street, 60 FPS).
- **iGPU limits left:** GPU thread waits ~1.8 s per 5 s in synchronous drains (DownloadMemory
  scheduler.Finish, CompleteReadbacks), ray-cast fences 30 ms late, ~27k write faults/5 s in the
  cloth window 0x1110100000, treasure cs 8f067540 dispatched 422x/frame (1x1x1 groups).
- **Garbage tables, more data (04:15):** a fault report (Rasterizer::ReportSrtFault, runs after a
  walk that faulted: every table of the walk, now and 3 ms later) with 4 cores + Sleep +
  GOW3_FINISH_AT_FLIP=1: VS d701fef2 / e467c702 user data point at 0x1027008f40 / 0x1027008fc0,
  which hold floats (constants), not the expected pointer table; unchanged 3 ms later. So the
  draw's user-data pointer refers to per-frame memory the game had already reused when the lagging
  GPU thread got to the draw (not a table being written during the walk). CE/DE counters are
  stricter than hardware (the CE waits whenever it is ahead), so not a constant-RAM ring overrun.
  Next: which fence lets the game recycle 0x1027xxxxxx early (compute-queue EOPs processed ahead
  of graphics? deferred labels?).
- **Job sleep default (04:30):** on for every PC (UNCHARTED_JOB_SLEEP=0 / launcher Spin turns it
  off). 3 x 2 min U3 on all 32 threads with Sleep: no walker faults, 60 FPS, 2.2-2.6 cores in all
  (6.4 with Spin, U1).
- **GPU_WAIT_PATCH loop (U3 0xe5f610):** submits each entry's command buffers and, only when a
  flag byte (+0x34 of a global at 0x18c6280-ish) is set, spins until the entry's label (+0x20)
  equals the expected value with the 2 s timeout we NOP. Not the per-frame wait; FrameBegin's
  m_gfxEopTick check relies on something else. Open lead for the early-recycling question.

## 17 October 2026 (day): cloth fix for both solvers, 4K CPU work, MT compute fences

- **U3 Sully shirt (third step):** the 128-thread solver cs 809864d3 has the same convergence loop
  as 1c957b01 (exit condition read from lds[0], rewritten at the top of the next pass; 17 barriers
  in the loop without the barrier-region rule, 47 in 1c957b01 with it). BarrierRegionShader default
  now {1c957b01, 809864d3}, cache suffix _lds4 (vk_pipeline_serialization.cpp). The earlier
  "809864d3 hangs with the rule" came from a GOW3_DUMP_SHADERS run that stalled before 809864d3 was
  even compiled. With the rule: 29 sites, no hang standing or walking. Diagnostics:
  GOW3_CLOTH_TRACE=1 (who reads the solvers' outputs, missed-barrier check; no GPU copies, unlike
  GOW3_CLOTH_PEEK, whose copies add sync points), "Cloth outlier"/"Cloth jumps" lines (the cloth is
  body-local, |x| ~2, so the old >1e4 test never fired). Chain: solver s6 + previous s29 -> skinning
  cs 7d7e7206 -> draws; every solver -> 7d7e7206 read had a barrier.
- **Static ranges (buffer_cache.cpp UseArenaForStatic):** U3's crowd skinning cs 8f067540 (~1,470
  dispatches/frame) reads 5 static mesh arrays per character (1-8 KB); the stream path (<= 16 KB,
  read-only) re-copied them on every bind: ~790k copies/s, 12 GB/5 s (GOW3_STREAM_STATS=1). A range
  >= 1 KiB bound in 3 submissions now lives in the arena (CPU write tracking; demoted for good once
  CPU-modified); clean ranges take a fast path (GetArena only); tracked in 64 KiB pieces in the
  barrier tracker. Never where the runtime writes through the backing view (labels, readback
  landings, image downloads: MemoryManager::BackingWritten bitmap), where GPU data may share the
  pages (MayBeGpuWritten), or in kept-writable fiber stacks. GOW3_STATIC_BUFFERS=0 off.
- **Compute RELEASE_MEM fences:** the plain store faulted on pages with GPU data and the fault
  fetched old GPU bytes on the second queue, waiting for the GPU (~1.7 ms, ~40/s, 6% of U3's GPU
  thread; found with GOW3_FAULT_STATS write RIPs). Now through TryWriteBacking + NoteLabelWrite when
  MayBeGpuWritten(page) (PM4CmdReleaseMem::SignalFence(irq, gds, write_mem)); unconditional
  bookkeeping cost U2 (~45k releases/s) 4%.
- **Other GPU-thread cuts:** compute dispatches merge overlapping/adjacent bound ranges before the
  barrier tracker (ResetBindings; draws keep the old path, the sort cost U1/U2 3-4%); lock-free
  64 KiB GPU-window bitmap before the GPU-written range set's lock (SRT walk); equeue notify only
  when an event becomes pending and a thread waits (m_waiters).
- **Multithreaded rendering:** RunComputeReleaseMem on stage B defers the fence through
  SignalAfterHostCopies like end-of-pipe fences (NoteLabelWrite on stage B; the callback writes the
  value and raises the interrupt).
- **Results, 4K, fps_limit 120 (RTX 5080 / 9950X3D):** U3 checkpoint 63.9 -> 79, crowd 68.9 -> 90;
  U2 museum spot 84 -> 99; U1 94.5 -> 101. MT: U3 100.5, U2 118, U1 117. 4 cores, 1080p: U3 ST 90,
  MT 120 (cap). 5-minute U3 soaks at 1440p/60 (Spin), ST and MT: no errors.
- **Rejected:** same-submission stream copy reuse (12% hits, slower; GOW3_STREAM_REUSE=1),
  splitting the copy batch at fences (GOW3_COPY_SPLIT_KB), GpuRangesAdd memo (no gain).
- **Open:** U3 on the AMD iGPU (GOW3_GPU_ID=1) stops flipping 0-35 s into the level; also with the
  04:13 build. Not: static ranges, equeue, host arenas, COPY64, job sleep, async shaders,
  frames_queued 2. U1/U2 fine on the iGPU. lldb (scratchpad hangcatch.sh): GPU thread idle, no
  deferred signals, main thread polling a job counter (guest 0xd975b0, from 0x566c89), workers
  idle, frame submission thread idle (cond wait at guest 0xe5fb6b).

## 17 October 2026 (evening): raw mouse look (U2/U3) and keyboard/mouse button icons (all games)

- **Mouse look** (src/runtime_mouselook.c, installed by probe.c `install_mouse_look`): the entry
  of each game's camera input function is patched with a jump to a stub that calls a handler with
  (camera, look direction, stick). U3 0x4e42f0, U2 0x428f40 (image offsets, 1.02): yaw rotates the
  look vector about +Y, pitch goes through the game's own step function (U3 0x100ef90 on
  camera+0x7750, pitch at +0x7874; U2 0x4288d0, pitch at +0x9e60), scaled so a count turns the
  same angle both ways. 0.0011 rad/count x GOW3_MOUSE_SENS. U1 not done: its camera reads the stick
  through 0xb7430 (input at params+0x10, right X +8, Y +0xc); caller 0xb62a5 (vtable+0x98).
- **Button icons** (gpu/shim/gow3_prompts.cpp): all three games draw the button icons in their
  text from one symbol font, data/fonts/ps4/symbol3.fnt (BMFont text, 32 glyphs, 512x512 atlas;
  text tags like |X| |T| |move| map to glyphs; 1-30 and 40-49 are two drawings of the buttons).
  The atlas is a CPU-written R8G8B8A8 1D-tiled image (tile mode 13: 8x8 tiles in rows, Z order
  inside, verified against a texture-cache dump). U1/U2: one RGBA texture; U3: colour (alpha 0)
  and a separate alpha texture (grey in RGB). Recognised in TextureCache::RefreshImage by the XXH3
  of the guest data (U1 ad5ada34fd34c76b, U2 97779d0906f16551, U3 8d03f2ff457f032d +
  ae0c06306763d424); while keyboard/mouse icons are on, a redrawn linear atlas uploads instead
  (key caps from stb_truetype SDFs of the embedded DejaVu Sans, mice as analytic SDFs; cells not
  redrawn, e.g. Share, keep the original). runtime_pad.c publishes the labels from the live
  bindings (L2/R2 prefer the mouse) and the device used last (pad buttons, sticks past half way,
  triggers vs mapped keys, mouse buttons/movement); a switch calls TextureCache::InvalidateMemory
  on the atlas, which re-uploads at its next use. GOW3_PROMPTS=auto|keyboard|controller (launcher
  Controls -> Button icons), GOW3_PROMPTS_LOG=1 logs every 512x512 upload's hash.
- Test aids: GOW3_PROMPTS_LABELS=1 (labels with keyboard/mouse off), pad-file tokens kbm / pad
  (device use), GOW3_WINDOW_BACKGROUND=1 (window opens unfocused, behind other windows, never
  raised), the presenter dump trigger's `size W H` line (every cached image of that size).
- **In-world button pictures** (the Triangle beside a chest): separate BC1 pictures, 256x256 with
  9 mips, 1D-tiled, stored upside down, colour textures masked by shared "trans" alpha textures
  (hard circle, soft glow). Found with a texture-cache dump in the user's U3 session (presenter
  trigger `size 256 256`), then all of them by scanning the paks for 1D-tiled BC1 images with a
  uniform border (scratchpad pakscan.py): U3 hud.pak face buttons + L1/L2/R1/R2, U2 hud.pak and U1
  pak19/particle.pak the same four face buttons (identical in U1 and U2). The game's data equals
  the pak's for mip 0; the padded tiny mips differ (not deterministic), so they are recognised by
  the XXH3 of mip 0. gow3_prompts.cpp decodes mip 0, flips it upright, keeps the button and glow,
  paints over the symbol (face: inner disc; shoulder: the text box clipped to the button) and
  draws the binding in the symbol's colour (a mouse for mouse buttons), re-encodes every mip to
  BC1 (principal-axis endpoints, punch-through kept) and uploads linear mips. Offline check:
  scratchpad prompts-test.exe (gow3_prompts.cpp built with GOW3_PROMPTS_TEST) + runpics.sh.
- **Controller icons:** label set 1 for non-PlayStation controllers (SDL_GetGamepadType; letters
  from SDL_GetGamepadButtonLabel, so Nintendo pads get theirs): A/B/X/Y in Xbox colours (pictures
  recoloured), LB/RB, LT/RT, LS/RS, Menu, View; PlayStation pads keep the game's icons. Device 0/1/2
  = PlayStation / keyboard / drawn controller; GOW3_PROMPTS adds playstation and xbox.
- **U1 mouse look** (0xb7430 orbit camera, stick-equivalent injection) is behind
  GOW3_MOUSE_LOOK_U1=1 until tested; GOW3_MOUSE_LOOK_DEBUG=1 logs its inputs once a second.
- **Not yet verified in game (17 Oct 21:47):** the in-world pictures and the controller icons
  (only the offline redraw); the text icons are verified in U1/U2/U3. Not done: U2's shoulder
  pictures (not found by the scan), the stick and d-pad pictures.
- **Corrected picture hashes + HOLD pictures (17 Oct 23:37):**
  - The pak scan's 32-byte aligned start offsets were up to 24 bytes late, so the first U1/U2 face hashes were wrong. The scan now walks back to the first border block.
  - Triangle, Circle and Square are the same in all three games (5db884aa…, 7af1abf6…, 9edd4f2f…). Cross differs: U1/U2 2bf5d90e…, U3 305c2c73….
  - The flat "HOLD" buttons are 512x512 BC1 pictures with two press positions. They were found live in the user's U2 session with the new dump-trigger lines `max W H` and `range LO HI` (up to 256 images, any format), and they match U2 hud.pak 0x828c98.
  - The HOLD Triangles are now swapped: U1/U2 25d954ce… and c96e34fb…, U3 4181f3ae… and 07ff3d0e….
  - The redraw covers the symbol out to its farthest saturated texel, so the old symbol's tip no longer shows.
  - Still the game's own: the U3 HOLD Circle/Square (decode garbled), the U1 shoulder HOLD pictures, and the U2 HINT d-pad picture.

## 18 October 2026 (night): FrameBegin m_gfxEopTick crash, aim sliders, in-game controls

- **m_gfxEopTick (FrameBegin, fg-draw-mgr.cpp:234):** most reported crash (GitHub #11 is another
  port's log; #12 x2 U1 RTX 3050/5060, #17 U3 RTX 3050, the user's U2 17 Oct 21:52). Logs show
  40-55 ms stalls just before (compiles, GPU waits). The timestamp is not an EOP label: the game's
  GPU event thread (ProcessGpuEvent, U2 0xc1e080, store at 0xc1e4cc) sets it on the end-of-pipe
  interrupt after checking the frame marker (Data64 EOP, m_frameWriteBack). Not reproduced on the
  desktop: held/lagged readbacks (GOW3_TEST_READBACK_HOLD/LAG), command thread stalls
  (GOW3_TEST_CP_STALL), late timestamps or interrupts (GOW3_TEST_LATE_TIMESTAMP/IRQ) only slowed
  the game (it waits); delaying 64-bit EOP data alone asserted m_frameWriteBack instead (test
  artifact). Overtaking signals while a fence waits for its readback: one WRITE_DATA and one
  flip interrupt per frame (GOW3_TEST_OVERTAKE_LOG). Fix: FRAME_BEGIN_WAIT_PATCHES (probe_uncharted.py,
  all three games): the assert's 59-byte path becomes a pause loop until the timestamp is set (up
  to 50M iterations), then continues. U2 with it: 59.6 FPS, no change in normal play.
- Aim sliders: AIM_RANGE_PATCHES U1/U2 5x, U3 4x (bench-tested by poking the value and stepping
  the slider). In-game Controls tab and controls in the settings file (see memory
  project-ingame-controls).

- **Hitch analysis (18 Oct night):** GOW3_PROFILE=<big> GOW3_PROFILE_STALLS=1 (and
  GOW3_PROFILE_GUEST=NdJobWorkerThread) writes the stacks every profiled thread had during each
  frame over 40 ms (dumps/stall_<n>_<thread>_stacks.txt). "Slow submit" (vkQueueSubmit > 8 ms),
  "Slow shader compile" / "Slow shader wait" (> 8 ms on the GPU thread) are logged. Findings: the
  game's job workers idle in hitches, the GPU command thread is the bottleneck: (1) compute
  shaders translated on the GPU thread at first use because NVIDIA (pipeline binaries) used the
  on-demand start without warm-up - fixed, background warm-up is the default now; (2) one
  vkQueueSubmit held 70 ms in the NVIDIA driver (kernel calls) in U2; (3) streaming frames upload
  ~115 MB of new images in one frame (resident/protect/image create ~16 ms). The user's 12.7 min U2
  session: 12 hitches over 40 ms, 1 with compiles.
- **Menu shortcut (GitHub #15):** Gow3Settings menu_shortcut (lt_r3 / view_menu / keyboard), in the
  Controls tab; the menu title shows the current one.

- **Draw distances (U2/U3):** debug-menu variables (dmenuvar2.py: item +0x38 = var, often
  `mov eax, OFF; add rax, [slot]`): render settings U2 0x1247b40 / U3 0x18c6280 (+0x150 temporal
  fade scale, +0x168 cutscene fade scale, +0x180 "Fade Distance Scale", +0x844 cascade fadeout),
  light settings U2 0x1162820 / U3 0x1b3c130 (+0xc0 max sun shadow dist, +0xc8 point/spot, cascade
  override and 4 distances at +0x218.. U2 / +0x190.. U3). Defaults 0.58 / 0.5 / 0.58, 40 m, 40 m.
  src/runtime_gamesettings.c recognises them by those defaults and writes the menu's percentages
  twice a second (Image tab, fade_distance / sun_shadow_distance / light_shadow_distance). Visible
  effect not confirmed: the bench saves (U2 shop interior, U3 chapter 2 rooftops) show no change in
  draws or screenshots; the user is to try them where pop-in shows.

- **Ultrawide:** the render resolution patch (resolution_patch, display settings sizes) at a 21:9 or
  32:9 size is all it takes: the games compute the projection from it (wider view, Hor+), the HUD
  and pause menus lay out fine, the aim focus fix maps the crosshair box (0.3750 -> 0.5000).
  Tested U1/U2/U3 at 2560x1080 and U1/U2 at 3440x1440 (60 FPS, no asserts). Launcher: Aspect ratio
  (ASPECTS, aspect_resolution); probe_uncharted.py steps ultrawide sizes down in the same shape.

- **Temporal anti-aliasing (18 Oct morning):** ported from reference/uncharted_pc_supermedo (GPL v2):
  GOW3_VELOCITY_PS=f4b18264 (the motion blur's camera velocity pass, 960x540, same in all three
  games) gives the camera: CameraMotion::OnVelocityConstants reads its constant buffer at the draw
  (z = c9 / (depth - c8), view = (z (c0 u + c2), z (c1 v + c3), z), rows c12..c23 to world, c40..c55
  previous clip x/y/w), camera_motion.comp mode 2 writes the motion vectors (pixels, previous minus
  current). Scene depth = the frame's first pass at GOW3_SCENE_SIZE (probe_uncharted.py passes the
  patched render size) that is depth-only or draws RGBA16F (U2 #0: D32S8 + RGBA16F 0x11478e0000).
  GOW3_UPSCALE_BEFORE_CS=aec539f0: AA runs on the lit scene before the post. Native AA only
  (preset 0, the upscaler's output stays its "native" 1920x1080 so Scaled() is false; the images'
  own sizes carry 1440p/ultrawide). taa.comp's depth test gets the velocity camera too
  (TaaDepthParameters: proj = (2/c0, -2/c1, c8, c9), previous view z = previous clip w row; the
  reference left it zero, which rejects all history). Near = |c9/c8| (U2 0.24), vertical FOV =
  2 atan(|c1|/2). The native path now dumps too (GOW3_DUMP_TRIGGER: input/motion/output/depth).
  Measured (8-frame dumps, scratchpad taatest.sh + taaeval.py): top-5% flicker pixels U2 0.043 ->
  0.019 (TAA) / 0.018 (FSR 3.1), U3 0.137 -> 0.050, U1 0.071 -> 0.038, U3 3440x1440 0.103 -> 0.042;
  turning: motion +51 px/frame for a right turn, near zero on Nate (orbit pivot), no trails. RTX
  5080 U2 4K: 120 FPS cap with TAA/FSR 3.1/off. Menu: Image -> Anti-aliasing (temporal_aa_available
  set by the renderer); launcher Display -> Anti-aliasing (ini "upscaler", default off; probe adds
  upscaler=off to a settings file without it, the renderer's own default being FSR 3).
  FSR 4 needs gow3_pc/fsr4_shaders (native model; listed only when present). DLAA needs
  gow3_dlss.dll (gpu/dlss_bridge, MSVC + NVIDIA DLSS SDK) and nvngx_dlss.dll beside Uncharted.exe.
- **DLAA and FSR 4 (18 Oct, 08:00, downloads approved by the user):** tools/fetch_aa_files.sh
  fetches the DLSS SDK v310.9.16 files (headers, nvsdk_ngx_s.lib, nvngx_dlss.dll: Authenticode
  valid, NVIDIA Corporation) into downloads/ and builds the bridge with MSVC (Build Tools 2022, CMake
  "Visual Studio 17 2022"; Vulkan headers copied alone from CLANG64), and the FSR 4 v07 INT8 native
  model (1080/2160 tiers) into gow3_pc/fsr4_shaders. package_windows.sh ships them with
  licenses/nvidia-dlss, gow3-dlss-bridge and fsr4; the workflow runs the script (continue-on-error).
  Measured: DLAA U2 top-5% flicker 0.038 -> 0.013, no trails turning; FSR 4 U3 0.119 -> 0.050,
  colours unchanged. RTX 5080 U2 4K: DLAA 84 FPS vs 120 off (GPU thread busy 99%: the model's
  GPU time, preset 0 = NGX default).

- **Cutscenes fast/out of sync above 60 FPS (user, U3, 18 Oct 08:30):** the movie player shows a
  frame per vblank; GOW3_VBLANK_HZ=0 (480 Hz) with limit 120 sped it up. Fix after the reference
  port: shim/videodec2 stamps Libraries::Videodec2::last_movie_frame_ns on each sceVideodec2Decode;
  PresentThread (videoout/driver.cpp) gives 60 Hz vblanks and flips while a frame was decoded in
  the last 300 ms (log "VideoOut: movie playing, 60 Hz vblank" / "movie ended"). The user's U3
  session showed 64-75 FPS at ~350 draws/frame with the 480 Hz vblank. Built 08:36, awaiting the
  user's test (they were playing; not bench-tested).

- **U3 rooftop freeze (18 Oct, FIXED):** hangprobe.py (thread RIP histogram + user/kernel time +
  register target protection) caught a guest thread 2.2-2.6 s of 3 in the kernel on `andq $-4,
  0x28(%rbx)` (e5dfbd, a fresh 0xf2a0 ray-cast batch at 0x1023900000, page NOACCESS): a write
  into a page a pending buffer readback read-protects (PageState::Perms: read watchers block
  writes) found no GPU bytes under it and returned; nobody landed the (finished) readback
  because only the GPU thread polls and it was idle waiting for the game (SubmitDone) -> fault
  loop; other workers spun on the ray-cast lock (ea5fc0). Fix: page_manager.cpp counts
  same-page faults per thread; 4 in a row on a read-watched page -> Rasterizer::ReleaseStuckPage
  -> BufferCache::ReleaseStuckPage (on the GPU thread via SendCommand): PollReadbacksNow, else
  CompleteReadbacks(true, newest) (submitted), else UnwatchPage. Logs "Stuck fault N: ...".
  Rooftops: no freeze in 10+8 min (froze in 0.5-5 min before); ~32 events/s, nearly all
  "waited for the GPU". Presenter frames-ahead wait now polls readbacks every 0.25 ms
  (Semaphore::WaitFor; GOW3_POLL_WHILE_WAITING=0 off). GOW3_OPEN_RAYCAST_READBACKS=1 (opt-in):
  ray-cast EOP results without page protection (no pacing gain measured).
- **Frame pacing (18 Oct):** GOW3_FRAME_LOG=<file> logs guest flip submits (S) and presents (P);
  scratchpad pace.py gives FPS/median/p99/1%/0.1% lows. The "75.0 FPS" seen at every 120-limit
  setting was the user's 75 Hz monitor with VSync (FIFO): frames queued behind VSync and the
  guest was held back in bursts (2-3 frames of 6 ms, one of 25-35 ms). Frames queued 2 and
  multithreaded rendering did not change it (the GPU thread was 22-25% idle with 2). Fix
  (videoout/driver.cpp PresentThread): in the unlocked mode (480 Hz vblank) with VSync and a
  limit >= the display refresh (30-120 Hz, Gow3DisplayRefreshRate exact), the vblank and the
  present slots follow the display; switching live. U3 rooftops and U2 museum at 120+VSync on
  75 Hz: 75 FPS, present p99 14.8 ms, 1% low 66 (was 32 / 22), 0% over 20 ms. Limits 75/90/100
  added. Also measured: SubmitDone -> WaitGpuIdle serializes guest submission with the GPU
  thread (kept: the reference found heap corruption without it).


## 18 October 2026 (afternoon): unlock all chapters; #18; other copies of the game

- **Unlock all chapters (UNCHARTED_UNLOCK=1, launcher Advanced, off by default).** All three 1.02
  executables keep Bluepoint's tweak flags (debug menu items "Unlock Crushing/Brutal difficulty by
  default", "Unlock Chapter Select by default") in the struct behind U3 [0x1dd26a0] (+0x2c60/61/62),
  U2 (+0x2da0/a1/a2), U1 (+0x2c30/31/32; U1 reads them as one dword). The constructor sets Crushing
  1, Brutal 0, chapters 0, then copies Crushing and chapters from the *bp-ucctweaks* table. Patches
  (UNLOCK_PATCHES): chapters = 1 where the table is read, Brutal = 1 where it is cleared (U1 has two
  identical constructor copies, told apart by their call displacement). Verified on the user's saves
  (U1 at Plane-wrecked, U2 at 33%, U3 at ch. 3): Chapters menu reaches U1 22, U2 26, U3 22; Brutal listed in
  all three. 1.00: no match (option does nothing).
- **#18 (U3 device lost after the museum guard scene, RTX 5070 Ti):** both logs stop when young
  Sully streams in mid-play (sullivan-young, cloth cs 1c957b01, DMA fs 9eb7be58/4f83b1de/43573568/
  78f8ad3b); GPU stuck 2.2 s on one submission, then TDR. Same U3 executable as ours. My bench save
  (user's) loads at the checkpoint after it (anim-col-thrown-out), where Sully loads with the level:
  no hang on the 5080. Their first log loaded a checkpoint before it and hung within 10 s. Asked for
  their save + launcher-last-run.log.
- **Other copies:** 1.02 executables are the same across regions (#17's CUSA02826 U3 = ours: same
  size and relocation count); saves/caches go by TITLE_ID from param.sfo. Base 1.00 is the problem:
  only 4/8 U1, 2/10 U2, 2/9 U3 patches match (no m_gfxEopTick wait, NaN focus reset, job sleep,
  aim range, torch), and the mouse-look sites are 1.02 offsets with a byte check (off on 1.00).
  Dry-run checker: scratchpad patchmatch.py <uroot>...

## 18 October 2026 (evening): 0.2.2 fixes (#21, #22) and the GPU command thread profile

- **#22 (all three games at start, Windows 10 19041, i5-8600K):** FAST_FAIL_INVALID_SET_OF_CONTEXT
  in ntdll!RtlGuardRestoreContext after a handled fault at U1 0x4fc0b2 (`push rbx`) on a sceFiber
  stack in guest flexible memory (rsp 0x12002504a0), outside the TEB's stack. probe.c
  cover_current_stack(), first thing in vectored_fault: TEB StackLimit/StackBase name the
  allocation holding rsp (or the thread's own stack again). GoW3 has no fibers.
- **#21 (U3 Syria, 0.2.1):** PatchImageSharp asserted "FMask storage instructions are not
  supported" during a level load (garbage T#). Now a warning: the store is dropped (atomics give 0).
- **Launcher:** start and callback errors go to analysis/uncharted-user/launcher-error.log and a
  message box (pythonw has no console; a player's 0.2.1 launcher closed silently).
- **GPU allocation records** (image.cpp, for GPU fault reports) only with GOW3_GPU_CHECKPOINTS: they
  grew by every image and buffer made in a session and each free searched them.
- **Profile (U2, 1440p, RTX 5080, GOW3_PROFILE):** GPU command thread ~25% buffer bookkeeping
  (ObtainBuffer: SynchronizeMemory, GpuRangesAdd, protect), ~3% image frees in the kernel
  (D3DKMTDestroyAllocation2), ~7% interrupt delivery. On 4 cores (affinity 0x55) the game uses
  1.6 cores and its submissions wait 16-22% in WaitGpuIdle after sceGnmSubmitDone
  (GOW3_SUBMIT_WAIT_STATS=1; GOW3_SUBMIT_NO_WAIT=1 is a measurement only: +5% FPS, worst frame
  45 -> 18 ms, unsafe). Opt-in, no measured gain: GOW3_IMAGE_REAPER=1 (frees on a thread),
  GOW3_ASYNC_IRQ=1 (compute interrupts on a delivery thread). Multithreaded rendering on 4 cores:
  U2 108 -> 120 (cap), worst frame 50 -> 10 ms; U3 114.5 -> 112, U1 102 -> 103, pacing worse.

## 18 October 2026 (night): 0.2.3 (U1 weapon flicker, U2 focus NaN)

- **U1 weapons flat grey on some frames (user, GitHub #23):** only with multithreaded rendering (the
  user's A/B; my checkpoint run: 11 flicker frames/min with the draw pipe, 0 with the start script's
  override). Not anti-aliasing, not the live optimization toggles (texture/buffer groups off: same
  rate). The draw pipe already snapshots user data, the walked SRT tables and ring constants at
  stage A, so the race is elsewhere (not found; GOW3_PIPE_VERIFY=<n> reports tables that changed).
  probe_uncharted.py turns multithreaded rendering off for eboot.bin
  (UNCHARTED_U1_MULTITHREADED_TEST=1 keeps it). Live flicker counter: scratchpad gunwatch.py
  (flash = grey-blue ~(54,74,74) where both neighbour frames are near black).
- **U2 `m_lookAt.X() == m_lookAt.X()` (#24, and the user's 16 Oct session):** PlayerBase::UpdateFocus
  (U2 1.02 0x7272e0; point [player+0x2b0], velocity +0x2c0, target rbp-0x60, snap 0x72769a). The
  assert path now clears the velocity and takes the snap when the target is a number
  (FOCUS_NAN_PATCHES). Test (scratchpad focusnan2b.sh): GOW3_EXEC_WATCH=7272e0 gives the player in
  rdi; NaN written to player+0x2b0: without the fix the assert, with it the point is 150.11 again
  3 s later and tracks (155.9 after walking).
- **Frame capture** read buffer contents at guest address 0 (IsValidGpuMapping only checks 40 bits):
  now through TryReadBacking; unreadable buffers are listed as such.

## 18 October 2026, evening: ultrawide Hor+ (#20), #24 = game 1.00, launcher usability (unreleased)

- **Ultrawide (#20) fixed: Hor+ in all three games** (HOR_PLUS_PATCHES, probe_uncharted.py; only
  for render sizes wider than 16:9, UNCHARTED_HOR_PLUS=0 disables). The render camera holds
  zoom = 1/tan(hFOV/2) and aspect = h/w (U2/U3 +0x254/+0x250, U1 +0x294/+0x290); projection
  x = zoom, y = zoom/aspect. Every perspective read of zoom in the camera update and in the
  frustum-plane builder (culling) now gets zoom * min(1, aspect*16/9) through a 37-byte stub
  (vmovss k; vmulss aspect; vminss 1.0; vmulss zoom; jmp back) in a dead assert branch (cave
  starts with int 0x41, which the runtime treats as fatal). Sites (1.02 VAs): U1 0x2c982e,
  0x2c9ac7, planes 0x2ca4ca; U2 0x9dc8fe, 0x9dcb55, planes 0x9dd41d; U3 0x11c1107 (both infinite
  paths), 0x11c11e1, 0x11c1406, planes 0x11c1f21. Orthographic branches (U2 +0x280, U3 +0x268,
  U1 +0x2a4) untouched; U3's sub-pixel offset term at 0x11c15e2 keeps the old zoom (negligible).
  apply_patches gained multi-change variants (pattern, [(offset, original, replacement)...]),
  all or none. Static check: scratchpad horplus_verify.py (disassembly of every stub, no branch
  from outside into a cave). Bench (scratchpad uwtest2.sh, GAME=u1|u2|u3): 3440x1440 on/off in
  all three games, 5120x1440 in U2: wider at the sides, the 16:9 vertical view, edge objects
  drawn, no faults.
- **#24 reporter runs game 1.00** (entry/image sizes match our 1.00 exactly): the 1.02 pattern
  patches are skipped there (U2/U3 focus NaN, FrameBegin wait...). The launcher now asks once at
  Play when the game is older than 1.02 with no update next to it (confirm_without_update), and the
  launch log says so. Red-zone clobbering by Windows exception dispatch was ruled out on Windows 11
  (scratchpad/redzone/redzone_test.c).
- **Launcher:** pages scroll (the Advanced buttons had been hidden since 0.2.2 by the fixed 330 px
  body), more room, GPU-based Recommended preset (gpu_names/gpu_tier), plainer hints, recommended
  choices starred, Advanced tools grouped, double-click a game card to play, #25 direct start.

## 18 October 2026, evening: laptop work merged into this tree

The three laptop sections below were written on the user's ROG Zephyrus G14 (USB copy, G:\uncharted
here) and merged on the desktop on 18 Oct evening: a three-way merge (base: the 0.1.2 export) of the
nine files changed on both sides, the nine laptop-only files copied. Conflict choices: liverpool.cpp
keeps SignalAsync; buffer_cache takes the residency pool and the timeline semaphore; vk_rasterizer
keeps both hang fixes (desktop ReleaseStuckPage after 4 faults, laptop LandWatchedReadback at the
first) and WaitDrawPipeForCapture; image.cpp keeps the GpuAllocationRecords gate with the laptop's
slow-create log and SpareBlockKeeper; probe.c / probe_uncharted.py keep the desktop's later work. One
duplicated declaration block (old-base merge) removed; scratchpad merge_dupcheck.py found no other.
Not merged: D:\uncharted-cmd (command-thread session, unmeasured). Desktop check of the merged build:
U1/U2/U3 single-threaded and U2/U3 multithreaded at 60 FPS, no faults; U3 landed 10 readbacks
through the new watch path. Benchmark runs and scripts: analysis/laptop-bench/.

## 18 October 2026 (night, laptop): Radeon 780M optimization (RTX 5060 switched off)

Laptop: ROG Zephyrus G14 GA403UM, Ryzen 9 270 (8C/16T, 16 MB L3) + Radeon 780M (driver
32.0.21036.1002, Jan 2025), 16 GB, 2880x1800 120 Hz; D: is exFAT; Windows power mode Best
performance. **But G-Helper was in Silent (30 W total, 18 W CPU) and GPU Eco the whole night**
(found 07:50 by the RTX session): every number below is power-limited. The user switched to
Balanced (80 W) and GPU Standard at ~07:50; re-measured numbers follow in a later section.
Measured with a gated scratchpad bench (Continue to the latest save, stand still; keyboard and
mouse off; saves backed up and restored): Frame stats every 5 s, GOW3_PROFILE, GOW3_GPU_PROFILE.
The camera view at the checkpoint varies between runs (1100-1550 draws in U3), so CPU-bound
results are compared in draws per second.

**Fixed / added (all in the laptop copy):**
- **Update 1.02 never ran on the laptop**: exFAT holds no links, so the game view failed and
  every game ran 1.00 (no Bluepoint fixes; JOB_SLEEP, FRAME_BEGIN_WAIT, AIM_RANGE and torch
  patches unmatched). New: `--app0-overlay <dir>` (runtime_file.c): the update is laid over the
  game by the runtime, indexed once, names without case, merged directory listings; probe.c reads
  param.sfo through it. probe_uncharted.py `boot_view` copies what the prepare scripts read
  (executables, sce_module, sce_sys; ~90 MB in analysis/views/boot-CUSA02344-01.02). Used only
  when the link view fails (NTFS unchanged). Log: `Runtime: /app0 overlay ...: 172 files, 33
  folders`. game_update.py links folders first (exFAT fails before copying files).
- **Error 1450 at start (16 GB PCs)**: the 6080 MiB guest pool was a SEC_COMMIT section (the
  whole pool charged against the commit limit at start). Now SEC_RESERVE, committed per direct
  allocation / flexible map (`host_commit`), retrying up to 10 s (lock released while it sleeps)
  while a system-managed page file grows.
- **Flexible memory split (pre-existing, found by review)**: split_at kept the parent's phys for
  the right piece of a flexible VMA, so after a partial mprotect/munmap of flexible memory backing
  copies hit the left piece's pages and flex_free zeroed live memory (reproduced with a harness).
  Fixed for every backed kind. Candidate for random guest corruption on every PC.
- **AMD allocation stalls**: vkAllocateMemory takes ~0.6 ms/MiB on this driver (170 ms per
  256 MiB VMA block). New: `Slow image create` log; image.cpp SpareBlockKeeper holds a 64 MiB
  spare so the next images find a block (after an image burst; GOW3_SPARE_IMAGE_BLOCK=0 off);
  buffer arenas take resident ranges from 64 MiB chunks allocated ahead on a thread
  (GOW3_RESIDENCY_CHUNKS=0 off): level-start frames with >10 ms residency 10 -> 0. One ~170 ms
  image-block stall at level start remains (120 images in one frame).
- **Side-queue page fetches** (CPU writes beside GPU data): a timeline semaphore instead of a
  fence (vkResetFences is a kernel call on AMD: 6% of the U3 command thread).
- **U1 shader compiles**: synchronous since 14 Oct (dark interiors); the 15 Oct small-target
  rule (SmallTargetDraw) covers the LUT draw, so U1 compiles in the background again (froze
  1-2.4 s per new area; now worst frame ~120 ms). UNCHARTED_U1_SYNC_SHADERS=1 restores it.
  To check: U1 temple torch light.
- **Multithreaded rendering races** (pre-existing): readbacks landed twice (`AddDelta: Not enough
  watchers`, `FreeDeferred on an allocation that is not held`, STATUS_HEAP_CORRUPTION) in U3
  within 1-3 minutes on the laptop. Fixes: a shader capture for the command thread waits for the
  draw pipe to run dry before handling its fault (WaitDrawPipeForCapture); the pipe ring has one
  producer again (RunInOrder from the recording thread runs inline, from any other thread, e.g.
  an EOP flip signalled on a copy thread, goes through the command thread). The watcher assert
  is a logged no-op. GOW3_FEWER_DRAINS=1 (off by default): fewer draw-pipe drains.
- Diagnostics/tests: GOW3_RING_STATS=1, GOW3_GPU_THREAD_PRIORITY / GOW3_GPU_THREAD_CORE (no
  effect measured), GOW3_EQUEUE_SPIN_US=<n> (waiter spins before sleeping).

**Measurements (1.02, 1080p, fullscreen 2880x1800, job sleep on, frames queued 1):**
- Levels are CPU-bound on the GPU command thread: U3 Cartagena ~54K draws/s single-threaded (49
  FPS at 1100 draws, 35 at 1530), U2 ~57K (36 FPS at 1600), U1 ~95K (37 FPS at 2620). Menus are
  GPU-bound (U3 50-55, U2 42-50).
- Multithreaded rendering, final build (race fixes): U3 43.9 FPS vs 35.4 single-threaded in the
  same 1540-draw view (~68K vs 54K draws/s, +24%); U2 no change; U1 slightly slower (34.6 vs
  36.5). No readback crash in 4 multithreaded runs since the fixes (it crashed 3 of 3 U3 runs
  before them). GOW3_FEWER_DRAINS=1 is about as fast; left off. But 2 of 5 multithreaded U3 runs on the
  final build stopped flipping right after the level loaded (0 of 2 single-threaded): suggested
  off for now.
- 2 frames queued: menus +10%, but U2 levels 9 FPS and U1 27 FPS (ray-cast fences landed 71 ms
  late): keep 1.
- GPU (U3 level, 780M, 1080p): ~17-18 ms/frame; 60 FPS on the 780M needs ~900p + FSR 1.
- **Memory pressure (16 GB + 780M)**: the 780M's "video memory" is system RAM. During U2 the
  system had 177 MB of commit left (game ~4.3 GB private + ~5 GB guest section + GPU memory, plus
  ~3 GB of other apps), and that run had a 2.1 s frame. U3 stopped flipping right after the level
  loaded in 5 of ~10 runs late in the night (both single- and multithreaded, process alive, no
  log message): the desktop's 17 Oct "U3 stops flipping on the AMD iGPU" (job fibers waiting in
  an allocator). Likely made worse by memory pressure; not resolved. Close other programs; the
  RTX 5060 (own 8 GB) avoids most of it.
- Experiments that did not help or broke things: GPU command thread priority/core (no effect);
  routing RunInOrder from other threads through the command thread (hangs: the command thread
  can wait for the flip it would queue; reverted); GOW3_EQUEUE_SPIN_US (a run with it stopped
  flipping; left off by default).
- Command thread (single-threaded U3 level): compute 54% (dispatch binding 28%, compute release
  fences 19%: signal + interrupt + event wakeup ~16%, waiting for deferred signals 7%), draws 30%;
  BindBuffers/ObtainBuffer 24%, AMD driver ~10%. Per-draw constant copies were 23-29% of stage A
  with multithreaded rendering.

## 18 October 2026 (morning, laptop): U3 "stops flipping" on the 780M - cause found and fixed

The hang behind "multithreaded rendering off on the laptop" (and the desktop's 17 Oct "U3 on
the AMD iGPU stops flipping"). Found from two hung runs with full minidumps and lldb stacks
(scratchpad hang-catcher: no Frame stats line for 25 s -> MiniDumpWriteDump + `lldb -p`).

- **Where it hangs**: not "after the level loaded" but at the menu -> level-load transition
  (Continue): every laptop hang (7 overnight runs, 2 today) stopped in Frame stats window 5-6,
  before the loading screen's first window, right as cs 67921bad/c92150ba run for the first time.
  It hit single-threaded too (2 of ~12 overnight), with multithreaded rendering more often.
- **What hangs**: one NdJobWorkerThread sits on a single guest store (+0xea50b1, `movl $0,0x10(%r8)`,
  zeroing a new pool item) for the whole hang: 20-22 s kernel time, ~6 s user, i.e. an endless
  access-violation loop. Its page is PAGE_NOACCESS. Every other thread waits for that job: the
  main thread polls the job counter (WaitForCounter usleep loop, +0xd9756a), another worker spins
  on a guest lock the stuck job holds (+0xea611e), the GPU command thread sleeps in
  Liverpool::Process (no commands, no deferred fences), recorder and draw pipe idle.
- **Why the page stays NOACCESS**: it is the `watched` range of the only pending buffer readback
  (dump 1: tick 4316, watched 0x1065c56000+8 KiB, GPU piece 0x1065c56d60+1 KiB, the store at
  0x1065c57180 is just past it; dump 2: tick 4297, watched 0x1065c72000+8 KiB, store 0x1065c732e0).
  Read watches (EOP/ray-cast readbacks, "read-protected until copied") make a page inaccessible
  until the readback lands (PageState::Perms). A CPU **write** fault there
  (Rasterizer::OnWriteFault -> InvalidateMemory -> memory tracker) marks the page CPU-modified and
  reports "handled", but nothing lifts the readback's watch: the store faults again, and again,
  until the GPU command thread lands that readback. It only polls while it processes work or
  while fences are deferred to readbacks; here it had none (deferred_signals 0, cached GPU tick
  4314 < readback tick 4316 < current 4317: the GPU finished long ago, nobody looked). The guest
  submits nothing until that job finishes: livelock.
- **Why the 780M**: the readback is still in flight when the job initializes its item beside the
  GPU's 1 KiB result block (false sharing in one page); on the RTX it has normally landed during
  command processing. Multithreaded rendering records the readback later in the submission, so
  the command thread goes idle with it pending more often. Not memory pressure (no commit-limit
  messages in any U3 log; commit headroom fell to 270 MB in clean runs too), not RunInOrder, not
  equeue wakeups.
- **Fix** (vk_rasterizer.cpp/.h, buffer_cache.cpp/.h, page_manager.cpp/.h): after a write or read
  fault is handled, Rasterizer::LandWatchedReadback checks whether a readback still read-protects
  the page (PageManager::IsReadWatched) and, if so, lands the readbacks that watch it
  (BufferCache::LandReadbacksWatching -> CompleteReadbacks(wait, tick); on the GPU thread through
  SendCommand<true> for guest threads, inline on GPU-side threads). Watches start only after the
  readback's submission, so the wait is for work already sent to the GPU. The retried store then
  succeeds. Logs "Readback watch: a fault at ... landing the readback (n so far)" (first 8, then
  powers of two) and warns if a page is read-protected with no buffer readback owning the watch.
- **Results** (U3, multithreaded rendering, job sleep on, 1080p, 780M as GOW3_GPU_ID=0 with the
  RTX on; each run: boot, Continue, load, 40 s standing in the level, which covers every hang
  seen; the desktop's struck within 35 s of the level start):
  - Before (05:27 build): 2 of 8 runs this morning hung (07:46, 08:11), both at the transition;
    overnight 7 of ~25 U3 runs, 2 of them single-threaded.
  - After (08:43 build and the 08:48 rebuild that includes it): **7 of 7 runs clean** (08:43-08:48
    in Balanced 80 W, 09:12-09:18). In every run the new path ran 65 to 500+ times, starting in the
    transition window, and 1-3 times per run at exactly the two stores that livelocked in the
    dumps (0x1065c57180, 0x1065c732e0). No "no pending buffer readback" warning, no errors.
  - RTX 5060 runs on the same build (other session): 1-17 landings per run (the readback has
    usually landed before the guest touches the page), U3 multithreaded 60 FPS, no errors.
  - Level frame rate unchanged within noise (43.6-45.7 FPS at 1116-1535 draws, once 40.9); the
    long frames left are the level-start ones (shader compiles, the ~200 ms image-block stall).
- **So**: multithreaded rendering no longer needs to be off on the laptop for this reason (it gave
  U3 +24% overnight, measured in Silent mode). The command-thread session reported that every U3
  run of its own hung at level start before this fix.
- Tools (u3-hang session scratchpad): hangrun2.sh (gated run + hang catcher + per-second memory
  log + early stop), minidump.py, mdump.py (minidump reader: fibers, threads, memory), dis.sh
  (guest disassembly). Memory note: reference-hang-dump-analysis.
- **Follow-up (09:20, stopped by the user before verification)**: an independent review (handbook
  session) found a multithreaded race the fix makes more frequent: Liverpool::ProcessCommands
  drained the draw pipe once before its command loop, so a command queued after that check (it is
  also called inside the packet loops, while the recording thread is busy), or one behind a flip
  command that woke the recording thread, could land readbacks concurrently with it (double
  landing, as before the 18 Oct night fixes). liverpool.cpp now drains before each command. Built
  at 09:21 (out/gow3-probe.exe) but **not yet run**: the next session should repeat a few
  multithreaded 780M runs and the RTX single-threaded 1440p case.
- The hang is not 780M-only: before the fix it also hit on the RTX 5060 single-threaded at 1440p
  (RTX session, run u3-rtx-st-1440-1018-075834: three level-start frames, the last with 27.9 ms of
  write faults, then no flips) and twice single-threaded at 1080p in the command-thread session
  (08:03, 08:07). Same symptom; consistent with the cause (the GPU still busy with the readback
  when the job writes beside it).

## 18 October 2026 (morning, laptop): power mode, 80 W numbers, session coordination

- **Power mode**: G-Helper was in Silent (30 W total, CPU 18 W) with GPU Eco from 17 Oct 23:58
  (found by the RTX session at ~07:50). The user switched to Balanced/Turbo (80 W) and GPU
  Standard. With the RTX on, the 780M is GOW3_GPU_ID=0 (`gow3-gpu-capabilities.exe --list`).
- **780M at Turbo, single-threaded, job sleep on** (build 05:27): CPU-bound levels barely moved
  (U3 34.9 FPS at 1517 draws = 53K draws/s vs 54K in Silent: one busy thread fits even in
  18 W). U3 900p + FSR: menu 55.5, level 36.0 at 1534 draws; U2 900p + FSR: menu 53.7, level 40.3
  at 1606 draws (65K/s); U1 1080p: level 36.5 at 2621 draws (96K/s). U2 at the user's 2560x1440
  on the 780M: level 27.2 FPS with 13 hitch windows (menu 37): use 900p + FSR there.
- **Output size is not the 780M's limit**: U3 in a 1920x1080 window was no faster than 2880x1800
  fullscreen (menu 54 vs 53); 720p + FSR locks the menu at 60. The GOW3_FULLSCREEN_RES experiment
  was reverted.
- **Launcher (with the user's approval)**: job_sleep "on", multithreaded off for u1/u2/u3,
  frames_queued 1 (backup: %APPDATA%\Uncharted Launcher\launcher.json.bak-20261018). Not changed:
  resolutions (U2 is still 2560x1440).
- **Coordination**: several sessions share D:\uncharted: analysis/laptop-bench/SHARING.md, a
  first-come-first-served lock for builds and game runs (lock.sh take/try/release/leave/show,
  QUEUE), EDITS.md for files in progress. A review of the U3 hang fix found a draw-pipe race
  (ProcessCommands drained once before its loop; a command queued after it, or behind a flip that
  woke the recording thread, could land readbacks while that thread did); the u3-hang session
  made it drain before each command (built 09:21, not run yet).
- **Remaining for 60 FPS**: levels need ~1.7x the command thread's throughput (U3 ~91K, U2 ~96K
  draws/s at their draw counts). The command-thread session's changes (D:\uncharted-cmd, RESUME.md:
  readback `rewritten` clipping, batched compute interrupts, a stream-copy thread, smaller items)
  are built but not measured. Multithreaded rendering gave U3 +24% before the hang fix; re-check
  with the hang and drain fixes. All sessions were stopped at the user's request at 09:23.

## 18 October 2026 (21:15): host crash in TextureCache::FindDepthTarget fixed

The user's U2 session on the merged build ended after 38 min: `Fault: read access to 0x7dd5edd0 at
Uncharted.exe+0x11474b` (GPU command thread; FindDepthTarget <- BeginRenderingFull <- DrawRecord).
FindDepthTarget held `Image& image` across `slot_images.insert` (the stencil placeholder); SlotVector
Reserve moves every image and frees the old storage when it grows, so `image.image_uid` (rbx+0x58)
and `image.FindView` read freed memory. Pre-existing (not from the laptop merge). Fixed by fetching
the image again after the insert; ResolveDepthOverlap had the same pattern (`cache_image` used after
inserting the recreated depth image) and is fixed too. ExpandImage, FindImage and ResolveOverlap
fetch references after their inserts (fine). Smoke: U2/U3 60 FPS, no faults.

## 18 October 2026 (21:35): U3 freeze = endless nested fault in the stuck-page check (since 0.2.0)

The user's U3 froze (0 CPU, 146 threads); lldb (scratchpad freeze-u3-stacks.txt): the GPU command
thread nested in GuestFaultSignalHandler -> cached_pages[addr >> 12] -> fault -> the same handler,
again and again. Shader d701fef2's SRT walk read through garbage user data (0x6f36c37e6c..., then
non-canonical addresses, reported as fault address -1); after 4 faults on that "page" the 18 Oct
morning stuck-page rule (fault_repeats >= 4) indexed the page table with -1 >> 12. Fixed: the rule
only looks at addresses below NUM_ADDRESS_PAGES (as the laptop's IsReadWatched already did). The
walker then skips such loads as before. Smoke: U3 60 FPS, no faults.

## 18 October 2026 (22:00): long-session safety net

- **Run history** (scratchpad run_history.py over analysis/<game>/attempts, 1,019 kept runs): every
  user crash/freeze signature of 15-18 Oct is fixed or not seen since: FrameBegin m_gfxEopTick
  int 0x41 (U2 0xbd444a x9, U3 0xe3b8fa x11 bench; FRAME_BEGIN_WAIT_PATCHES), U1 host fault =
  my frame capture (fixed), ntdll+0x1ff2a (16 Oct MT work), GPU-library STOPs (14-15 Oct),
  FindDepthTarget and the nested-fault freeze (fixed today).
- **Fault-handler depth guard** (probe.c vectored_fault): a fault nested more than 8 deep inside
  gow3gpu_handle_fault reports and stops instead of recursing until the stack runs out.
- **Hang watchdog** (probe_uncharted.py hang_watchdog + tools/hang_report.py): no Frame stats line
  for 30 s (UNCHARTED_HANG_SECONDS; needs diagnostics) -> every thread's state from the launching
  process: StackWalk64 with the modules' unwind tables (module+offset), guest rbp chains, thread
  names, grouped by stack -> analysis/<game>/hang-<time>.txt (newest 10; ~20 KB, 0.2 s), copied into
  the run's attempts folder, named in boot.log after the game ends; minidump in dumps/ in the source
  tree (UNCHARTED_HANG_DUMP; ~340 MB, newest 2). Not written into boot.log while the game runs: the
  game writes through its own file position and overwrote appended text. Launcher: "The game
  froze" message whenever a report exists (also after STOP). Tested by suspending a bench game
  (scratchpad suspend_game.py): report + resume clean; offsets symbolize with llvm-addr2line.


## 20 October 2026: U1 0x64011f crash (red zone) and the nested-task leak

- **"execute access to 0" called from 0x64011f** (U1 1.02; #27, #30, #42, #62): `call rcx` at
  0x64011d walks a constant-update table (0x28-byte header, 0x30-byte entries, the 16-bit word at
  entry +0 selects a member function pointer). The table is filled by a leaf function (U1 0x63df10,
  U2 0xd645f0, U3 0x12d6cf0; same code in 1.00) that keeps running entry indices and pointers in
  the red zone ([rsp-4] .. [rsp-0x2c]). A table that crosses into a GPU-tracked, write-protected
  page faults inside it; the Windows x64 exception frame may be built right below rsp, a counter
  comes back wrong, an entry keeps a stale word and the walk calls null. Measured on this PC
  (Windows 10.0.26300, scratchpad cue/deep.c): the dispatch writes nothing above rsp-1648, which is
  why the bug never showed here. Fix (probe_uncharted.py CUE_PRETOUCH_PATCHES,
  UNCHARTED_CUE_PRETOUCH=0 turns it off): the function's first 5 bytes jump to a stub in a dead
  assert path nearby (still int 0x41 first) that does `lock or byte [p], 0` on every page of
  [rdi, rdi + 0x28 + 0x30*edx), then the moved pushes and back. Stub tested standalone
  (scratchpad cue/stubtest.c: a leaf with a red-zone value, a handler that zeroes rsp-48..rsp: the
  value is lost without the stub, kept with it, faults move into the stub); U1/U2/U3 run at 60 FPS
  with it, no faults.
- **Host memory leak** (liverpool.cpp): nested IndirectBuffer / IndirectBufferConst tasks stop at
  final_suspend and were never destroyed (only top-level tasks were). `task.handle.destroy()` after
  the three wait loops. U3 bench: private bytes +344 MiB in 135 s before, +26 MiB after (flat in the
  last minute), 60 FPS both.
- **Device-loss report from Semaphore::Refresh** (vk_semaphore.cpp; GitHub #67: the loss was first
  seen reading the master semaphore counter): ErrorDeviceLost there now dumps the GPU checkpoints
  and the device fault info before the assertion, as the waits and the submit already did.
- **Effect shader variant storms** (GitHub #71/#79; shader_recompiler/resource.h
  IsGarbageEffectBuffer, Shader::gpu_mapped_extent set in the Rasterizer constructor;
  GOW3_EFFECT_SHARP_FILTER=0 = old): cs 8f067540 (effects + U3 crowd skinning) and 7d7e7206 read
  V#s from table slots that hold floats for some effects. BindBuffers bound them as null, but
  StageSpecialization and the translation still saw their random stride/format: every new float
  set made a variant plus a compute pipeline on the GPU thread. Our cache: 8f067540 1,177
  variants (all 9 slots with 450-590 distinct stride/format values), 7d7e7206 364; the user's
  18 Oct U2 session had a 7.8 s frame with 1,112 compiles right after the garbage V#s started.
  BufferResource::GetSharp now returns Null for these two shaders when BindBuffers' garbage rule
  holds (unaligned, stride >= 8 KiB, > 64 MiB, or not fully mapped with num_records < 0xFFFF0000),
  so specialization, translation and binding agree. Checked: all 95 garbage V#s logged in the
  three sessions that made the variants are caught; the stride rule alone cuts the cached keys
  1,177 -> 399 (364 -> 84); U2/U3 from empty caches and warm, 60 FPS, no faults. The U2
  Treasures screen did not produce garbage V#s on 20 Oct, so no live A/B.
- **Pageable video memory** (GitHub #81, MX350 2 GB: `ErrorOutOfDeviceMemory` from the buffer
  arenas' vkAllocateMemory). The sparse arena buffer accepts only device-local memory on NVIDIA
  (RTX 5080: type 1 only; temporary "GPU: arena memory type N allowed" lines in
  BufferCache's constructor). vramfill test (analysis/test-tools/vram): NVIDIA refuses device-local
  memory at 15,360 MiB without VK_EXT_pageable_device_local_memory and hands out 24,576 MiB (the
  test's maximum) with it; the limit is per process (a second process holding 12 GB changes
  nothing); held device memory also charges system commit (13 GB held: +13.4 GB commit).
  vk_instance.cpp now enables VK_EXT_memory_priority + VK_EXT_pageable_device_local_memory when
  supported ("GPU: pageable video memory on"; GOW3_PAGEABLE_MEMORY=0 = off). U1/U2/U3 normal runs
  60 FPS, no faults. Test knob GOW3_TEST_VRAM_HOG_MB=N holds N MiB at device creation: with it
  (11,000 or 13,300, pageable on or off) U1 dies at start with STATUS_STACK_OVERFLOW (exit
  0xC00000FD) about 2 s in, before the first frame, with no access violation (GOW3_FAULT_TRACE=1)
  and no log line: cause unknown, test-only so far (a real 2 GB card boots). Open lead.

## 20 October 2026 (afternoon): PR #87 merged (job sleep on game version 1.00)

kuraeow's #87 (merged on GitHub as 5cc4ea0, applied to the local tree; backup of the previous
tools/probe_uncharted.py in the session scratchpad) adds a second form to two of the three
JOB_SLEEP_PATCHES sites. The PR calls it "another retail 1.02 build"; in our dumps the new forms
are exactly the 1.00 executables (game/CUSA02344, all three games: one match each; 1.02: none):
- re-check before waiting: `mov rax,[r13]` instead of `[r15]`; the 1.00 timed-wait wrapper
  (U1 1.00 0x362420) multiplies its argument by 1000, so `mov edx,10` is the same 10 ms as 1.02's
  `mov edx,10000` passed straight to the wait.
- submission: `cmp byte [rdi+0x59],0; je short` instead of `[rax+0x59]` + `je rel32`.
Before: 1.00 players got only the worker-loop part (workers slept, woken only by the 10 ms
timeout). Offline check (apply_patches on copies of the six executables, old vs new script):
1.00 all three parts patched in U1/U2/U3; 1.02 output byte for byte the same as before.
No game run (the user was playing). CHANGELOG "Next version" entry added.

## 20 October 2026 (afternoon): 10 player crash logs (downloaded with the user's OK)

analysis/test-tools/issues/i<issue>/boot.log; summary script analysis/test-tools/issue_summary.py.
- **4 of 10 = U1 0x64011f** ("execute access to 0 at 0", host #1 guest offset 0x64011f, same
  guest stack 0x641532 <- 0x501ea9 <- 0x56d027 ...): #78 (RTX 3050 Laptop 4 GB), #82 (RX 9060 XT),
  #85, #90 (RTX 5060 Laptop 8 GB). That is the CUE red-zone crash fixed by
  CUE_PRETOUCH_PATCHES (unreleased; 0.4.1 does not have it). The "crash after 10-30 min on 4-8 GB
  laptops" pattern is this crash, not video memory: no VRAM pressure in #90, #78 steady ~2.9 GB.
- U1 0x5f64f3 read of 0 (#59), U2 0xd09665 read of 0 (#61), U1 0x63f61e read of -1 (#34),
  RX 9070 boot "ASSERTION: root" + read of -1 at 0x4dd8ca (#80): open.
- Device lost: #57 (0.3.1, garbage 6755-layer image, fixed in 0.4.1 by PlausibleTarget), #76
  (RTX 5080 4K, U3 chapter 8): open.
- Performance: #61 (U2, RTX 4070 SUPER, 2880x1620) guest-thread read faults on readback pages
  445 per 5 s costing 426 ms (8.5% of the time), "23590 recurring readback pieces"; FPS min 25.7,
  worst frame 6.5 s. #34 (U1, older build) 1,850 compiles, worst frame 2.5 s.
- Arena residency is never released (EnsureResident only adds; nothing removes resident_ranges,
  also not on unmap): real, but not what these logs crash on. Keep as a later 4 GB-card item.

## 20 October 2026 (afternoon): performance work in progress (2 workers), notes

- Worker A (done, built, not deployed): trigger files polled at most every 250 ms (TriggerPollDue in
  vk_rasterizer.cpp; vk_frame_capture.cpp; vk_temporal_upscaler.cpp), stream-copy stats clock once
  per submission, write-fault statistics only with GOW3_FAULT_STATS, small-GPU-write stats
  lock-free once printed, IsComputeMetaClear cheap tests first, CopyBuffer watch lookups only with a
  watch range, PublishGraphics early return + failed_graphics map, fetch shader data assigned in
  place (vk_pipeline_cache GetProgram out-parameter, FetchReset), getenv cached in runtime_kernel.c
  (every guest clock read), runtime_audio.c, runtime_sema.c. Reviewed by me against
  analysis/backups/src-before-perf-20oct: behaviour-identical. Also mine: shim/common/debug.h
  TRACE_* no longer evaluate their argument (texture_cache formatted a string per image refresh).
- Not done (risk found on the second check): dropping the dead 8f067540/7d7e7206 variants from
  players' caches by a per-shader meta version. New variants would reuse the old perm_idx names
  (_0.spv, compute key HashCombine(hash, perm)), so a stale pipeline binary could pair with a new
  module unless pipeline binaries are keyed by the SPIR-V. Needs that check first.
- PR #84 (open), new concern, checked twice: LoadShaderMeta never loads motion vertex shaders
  (session-local buffer device addresses), and #84 sets sel.motion for every Uncharted G-buffer
  draw (stencil + depth write, one target) -> every G-buffer vertex shader variant is compiled
  again in EVERY session (not only the first). Decide "character draw" before pipeline selection,
  or make the motion VS address-independent (push constant / descriptor) so it can be cached.

## 20 October 2026 (evening): US copy (CUSA02320) tested, E:\uncharted usa

The user's US PKGs (base 43 GB + update 1.02, 1 GB): base unpacked with tools/pkgtool PkgTool
pkg_extract to E:\uncharted usa\CUSA02320 (3 min, 59 GB, same uroot layout as CUSA02344); the
update with the launcher's installer (tools/install_update.py) as CUSA02320-patch (separate
folder, the layout of #43/#29/#58/#73).
- PkgTool pkg_extract leaves out the PKG's metadata entries: the base had no sce_sys/param.sfo,
  playgo-chunk.dat/.sha, playgo-manifest.xml, shareparam.json, so install_update.py refused the
  folder ("Choose the game folder first"). Added them with install_update's own entries() +
  pkg_extractentry (METADATA list). A player who unpacks the base PKG himself hits the same.
- With the current build (deployed before-perf exe): U1, U2, U3 all start (title, 60 FPS) through
  the links view (E:\uncharted usa\.uncharted-views, eboot = the update's, sha1 d81e699f...) and
  with UNCHARTED_TEST_OVERLAY=1 (runtime overlay, 171 files). Without the base's metadata the game
  still starts (playgo-chunk.dat comes from the update's sce_sys). So the US start crash does not
  reproduce here; cause still unknown (#82 is US + 0.4.1 and got in-game).
- Idea for players with PKGs: launcher "Install game from .pkg" (base: pkg_extract + metadata).
- 20 Oct evening: launcher Advanced -> FOLDERS -> "Install game from .pkg" (launcher
  install_game_pkg -> tools/install_update.py install_game; CLI `--game <pkg> <folder>`): pkg_extract
  into <folder>/<TITLE_ID>, metadata entries added to uroot/sce_sys, base files checked, update PKG
  refused (param.sfo CATEGORY gp; base is gd), existing install refused. Tested with the US PKG
  (2 min 49 s, then removed). US 1.02 executables are byte-identical to EU 1.02 (all patches apply).
- 20 Oct evening, US gameplay check (new build out/gpu, GOW3_TEST_CHOICE=0 answers the port's save
  list dialog, new test knob in shim/gow3gpu.cpp): New Game in all three games from the US copy:
  U1 chapter 1 boat (60 FPS, worst 17.2 ms), U2 chapter 1 train cliff (60 FPS, 4+ min), U3
  chapter 1 bar fight (60 FPS; one 65 ms frame at the level load). No faults.

## 20 October 2026 (evening): hybrid-laptop freezes = present under the submit lock (fix built)

- Laptop (7840HS, NVIDIA laptop GPU + 780M; the 780M drives the 2560x1600
  165 Hz panel, frames copied across): 17:30 build, 1080p windowed: U3 median 38 FPS (min 24),
  freezes to 0.82 s; U1 Continue median 47 (min 32), freezes to 0.61 s; menus 46-184 ms freezes.
  In the freezes the GPU command thread was not idle, in no counted wait, ~31 ms on the CPU.
- Cause in code: Presenter::Present / PresentLoadingFrame called vkQueuePresentKHR holding
  Scheduler::submit_mutex (present queue = graphics queue), so every SubmitExecution waited for a
  blocking cross-adapter present. Fix (built 19:21, out/gpu, sha1 1e31b0d7): a third queue of the
  graphics family for presents (vk_instance.cpp; Instance::SeparatePresentQueue; PresentLock in
  vk_presenter.cpp), GOW3_PRESENT_QUEUE=0 = old. New counters Gow3Stats::frame_wait_ns /
  submit_lock_ns (draw scheduler only) / acquire_ns / present_ns: "Presentation waits:" line per
  5 s and a "presentation:" line per Stall block (driver.cpp). AMD (1 graphics queue) unchanged.
- Desktop check (5080, screen on the card): U3 and U1 gameplay 60 FPS, worst 17.2 ms (U1 one
  38 ms), "presents on their own queue", present 1% of wall time, saves restored. Usual build
  (before-perf) deployed again. Backups: analysis/backups/main/present-queue/.
- Package for the laptops: analysis/laptop-pkg/fix-20oct-2(.zip, 34.6 MB): exe, present-queue.patch,
  LAPTOP-HANDOFF.md (pq-on/pq-off A/B twice in U3, U1, U2, stall stacks, 780M). Laptop share access
  failed both ways (laptop port 445 closed; this PC refuses the laptop: account restrictions).
- 5060 laptop: no results today; its session was offline at 19:40.
- Still open: U3 ~48 FPS ceiling on laptop CPUs (GPU command thread CPU-bound, ~54K draws/s).

## 20 October 2026 (night): laptop freezes = port on a USB exFAT drive

- Laptop: D: = NVMe SSD in a USB enclosure (ASMT 2462, exFAT); C: = internal NVMe.
  The port tree (D:\uncharted) and first the game (D:\game, /app0 overlay) were on D:.
- Present-queue theory disproven on the laptop (Presentation waits ~0% with and without it).
- Game on C:, port on D:: U3 steady 53-55 FPS but freezes to 0.67 s after the level load;
  U1 worse (28-60 FPS, 0.5 s freezes every window). Port AND game on C: (port copied to
  C:\Users\bilal\Desktop\uncharted\port, 3.2 GB): U3 1 frame over 40 ms (43.9), 0 over 100 ms.
- Freeze stacks (port on D:): GPU thread 61% waiting for the guest, ~13% in trigger-file checks
  (filesystem::exists/remove on D:, ~100 ms each); game flip thread waiting on a guest condvar.
- Launcher: slow_drive_reason / confirm_slow_drive (USB/SD/MMC/FireWire bus via
  IOCTL_STORAGE_QUERY_PROPERTY, removable, FAT/exFAT): warns once per port+game folder pair.
  Checked here: D: (WD My Passport USB HDD, exFAT) flagged, NVMe drives not. CHANGELOG entry.
- Open: make the port immune (async log writes, no trigger polls on slow drives): pinpoint run
  dport-guest-u3 requested; CPU-bound U3 ~47-54 FPS (NVIDIA GPU) / 41 (780M 720p): optimization worker.

## 20 October 2026 (night): GitHub #95 memory, proper fix (built, laptop test pending)

- #95 log (0.4.1): compute pipelines 926 -> 9,942 over the session (variant storm of cs 7d7e7206 /
  8f067540, fixed in the tree: laptop soak 40 -> 47). RAM flat in steady state on the laptop
  (8.46 GB private for the last 6 of 16 min) = the host leak fix holds.
- Remaining: buffer arenas never release (soak 320 -> 1,920 MiB in 16 min, one level change).
  Fix (fix-20oct-6, sha1 06725fb6, 21:40): BufferCache::ReclaimArenaMemory (per submission from
  Rasterizer::OnSubmit, works every 2 s): 1 MiB windows of guest GPU memory get the epoch of their
  last arena use (TouchArenaWindows in GetArena and SynchronizeDmaRange). Above GOW3_ARENA_LIMIT_MB
  (default max(768, heap/4)), windows unused for GOW3_ARENA_RECLAIM_AGE_S (60) are released oldest
  first down to 3/4 of the limit: skipped when GPU-written (gpu_modified_ranges, tracker), pending
  readbacks, lazy pages, or the GPU has not finished the epoch after their last use.
  ReleaseArenaBlocks: IntervalList::Remove (new, with a cut functor: Backing::SubRange mixes block
  and byte units), sparse unbinds into pending_binds, BDA page table entries zeroed, guest range
  marked CPU-modified (re-upload; static ranges demote via IsRegionCpuModified), memory runs
  quarantined until SubmitPendingArenaBinds, then reused first by EnsureResident (which also trims
  pending unbinds of ranges bound again). GOW3_ARENA_RECLAIM=0 off. Memory: line shows held / in
  use / given back. Risk: shaders in DMA "full" mode reading a released window see zeros (as for
  memory never made resident); mitigated by the 60 s age and the limit.
- Backups: analysis/backups/main/arena-reclaim/. Laptop runs: forced-u2/u3/u1 (limit 600, age 20,
  screenshots), default-u2 900 s, 780M u2.

## 20 October 2026 (night): CPU savings confirmed on the NVIDIA GPU; 780M memory placement

- fix-20oct-5 A/B on the NVIDIA GPU (U3, C: port), two pairs: GOW3_READBACK_POOL + GOW3_SKIP_COMPUTE_REBIND
  on vs off: ~1,030 draws 54.5 vs 50.9 FPS (+7%); ~1,250 draws 65.3K vs 62.0K draws/s (+5%).
- 780M (pq-on-1 log): sparse arenas allowed in type 0 (heap 0, 256 MiB, DeviceLocal), 1 (heap 1,
  19.8 GB system, HostVisible|Coherent), 2 (heap 2, 256 MiB, DeviceLocal|HostVisible), 3 (heap 1,
  +HostCached). The arenas went to the 256 MiB heap (overcommitted by 1-2 GB).
  fix-20oct-7 (22:00, sha1 c7ebe32f): GOW3_IGPU_ARENAS=system (integrated GPUs, opt-in) picks the
  largest HostVisible|Coherent heap (prefers HostCached), arena_host_mapped; reclaim limit there =
  max(768, min(2048, heap/8)). GOW3_WAIT_PAGE_READ (on): with host-mapped arenas and no copy queue,
  a CPU write fault waits for FetchableWindowTick then ReadFinishedGpuPage instead of a full
  download. Also: SrtPlanFor memo (32 entries), StaticRangeBits 18 -> 16.
- 780M U3 720p, fix-20oct-7 (same view, ~1,028 draws): default 43.9/45.1 FPS; GOW3_IGPU_ARENAS=system
  47.2 (and 44.4 at 1,195 draws = 53.5K draws/s), GPU ticks 3-3.7% -> 0.1-0.5%, 181 direct page
  reads/5 s, no faults -> made the default for integrated GPUs (22:21 build; =local: old).
  System + multithreaded (UNCHARTED_MULTITHREADED_U3=1 UNCHARTED_U3_MULTITHREADED_TEST=1): 55.1 FPS
  (min 53.9, worst 29 ms), stage B busy 62%, stage A waited 18%, no faults. 15-min MT soaks on
  both GPUs requested before changing the U3 multithreading default (#51 freeze risk).
- 780M U2 subtitle / pause menu flicker (user, watching): bis-1 system arenas yes, bis-2 local
  arenas yes, bis-3 READBACK_POOL/SKIP_COMPUTE_REBIND off yes, bis-4 laptop's pre-20-Oct exe
  (cc6f758b) yes = pre-existing, not today's work. bis-5 (NVIDIA GPU at 720p) / bis-6 (780M at 1080p)
  split GPU vs render size. System arenas made the iGPU default again (23:04 build).
- Desktop (user, 5080, usual build f0ec3cbf, U2, DLSS 4K render): aim blur back. Log: "Aim focus:
  crosshair box centre 0.25 -> 0.5 (render 3840x2160)" applied, "Tiny image readbacks: ~300/5 s"
  (last 0x1040340000). Read-only live check: scratchpad u2focus.py (global 0x13e42c0+0x35f8).
- U2 aim blur (user, 5080, 4K DLAA): live read (u2focus.py) of the focus value: alternates between
  2.06-2.18 (target, tracks aiming) and 5.637/5.641 (= the uncorrected box: the wall beside Nate,
  16 Oct analysis). FixAimFocusCentre wrote the corrected centre through the backing view only
  (not tracked): when another draw had uploaded the block earlier in the frame the GPU kept the
  old centre. Now BufferCache::WriteThrough (guest + GPU copy in stream order), backing fallback.
  Build 23:10 deployed to out/gow3-probe.exe for the user's U2 check (U3 uses the same path).
- 780M flicker: bis-5 (NVIDIA GPU at 720p) no flicker -> 780M-specific, pre-existing.
- U2 aim blur FIXED (user-confirmed 23:24, 4K DLAA): FixAimFocusCentre now writes through
  (BufferCache::WriteThrough). New per-5 s line "Aim focus (5 s): N binds; skipped ...; N corrected
  (N through to the GPU copy)": user session 300/300/300. Build 6a3bb583 (23:20) deployed.
- U3 d-pad prompt (user screenshot, museum chapter): picture 256x256 BC1 hash e8a2f49b992621fc
  (masks f63e4fb97d71d9ed / 31e0b53a6c874ec5), found with GOW3_PROMPTS_LOG=1 (test-env.txt, restored)
  and scratchpad promptsheet.py (reads the logged pictures from the running game, read-only).
  gow3_prompts.cpp Kind::Dpad + RedrawDpad: the bound key (GOW3_PROMPT_UP) on the lit orange arm,
  keyboard set only (controllers keep the picture). prompts-test harness takes "dpad"; rendered OK.
  Build 00718657 (23:39) deployed; user check pending.
  User-confirmed fixed (23:50).
- Laptop mt-soak-nv (fix-7, U3 MT, 15 min): FPS 60 most of the time (MT: GPU thread idle 23-27%)
  but dips: compute variant storms (cs 8f067540 / 7d7e7206 / 0c936dd6 each 100+ variants, compute
  pipelines 40 -> 733, a 918 ms frame with 136 compiles). 0c936dd6 had unmapped 1-3 GB V#s (bound
  null, but specialised). Fix (fix-20oct-8, 23:53, sha1 34d1b37b): IsGarbageEffectBuffer for EVERY
  shader (odd-looking V#s that BindBuffers would null read as null in GetSharp); effect family
  (now incl. 0c936dd6, IsEffectShader) keeps the strict rule; "variant N buffers" diagnostics at
  101-106 variants. f8 MT soaks (NVIDIA GPU, 780M) queued. Deployed on the desktop too.
- U3 museum red carpet (user): pre-existing (the pre-20-Oct build shows it too). Frame capture
  analysis/uncharted-user/captures/frame_20261020-235100_2.txt (scene pass #22: 98 draws, 125
  textures); cause not found yet.

## 21 October 2026 (night): U3 museum red floor = damaged shader cache pair (FIXED, verified)

- Symptom: the room behind the guard's door (young Drake, museum) shows a flat red floor on the
  desktop (RTX 5080) at any resolution/upscaler/draw distance; the laptop renders its green/yellow
  tile pattern. Not vendor, not depth: GOW3_PIPELINE_CACHE=0 (fresh compile) renders it right.
- Cause: mesh ib 0x119c453800 (96 indices), shading vs 91304408 (ps 34dde251). Shader metas are
  named HashCombine(pgm, perm_idx), binaries <pgm>_<idx>.spv; two separate blobs, binary first.
  20 Oct 08:14 a session rewrote 0x91304408_4.spv (another permutation's code, same as _5) but
  the rename of 0x000009133abab9f5.meta failed (warm-up reading it): the only .meta.tmp in the
  20,378-file cache. Every later start loaded meta(18 Oct spec) + wrong binary.
- Fix (vk_pipeline_serialization.cpp, cache_storage.*; backups *.before-metahash): ShaderMetaVersion
  13 = meta stores XXH3 of the binary written just before it; BinaryMatchesMeta rejects mismatches
  (and removes both files via DataBase::Remove on the IO queue); v12 metas still load when
  DataBase::WrittenTogether (no .tmp, mtimes <= 30 s apart). WriteVector retries the rename 20x5 ms
  and drops the temp on failure. Verified on the original cache copy twice: run 1 rejected exactly
  that pair, floor correct; run 2 no rejections, floor correct (build 5ecdab95).
- Also: per-GPU cache swap (SwapCache, worker-reviewed + LRU fix); GOW3_SKIP_IB_DRAIN knob (MT only,
  default off); invariant positions opt-in (GOW3_INVARIANT_POSITION=1, made no difference).
- Diagnostics added (all env-gated): GOW3_SKIP_FILE accepts index-buffer addresses; depth probe
  modes 3 (GEQUAL) and "4 n" (bias); GOW3_MESH_DUMP=<ib>[+_ALL]; capture "mesh" lines (worker).
  Test scripts: analysis/test-tools/carpettest.sh, carpetlive.sh.

## 21 October 2026: compile-storm leftovers in old shader caches (#92, #95)

- Issue logs: caches built before the storm fix hold thousands of compute pipelines: #92 23,845
  (RX 9070 XT; amdvlk64.dll read fault at 0x60 while warming up, 24,034 of 26,910 done), #95 9,942
  (332 s warm-up), #109 8,282, #74 7,562, #29 7,052, #85 4,484 (89 s). Desktop cache: 8f067540 had
  1,268 cached versions and 7d7e7206 383; no other shader above 35. The storm fix stops new ones but
  the background warm-up still built every old version at every start.
- Fix (vk_pipeline_serialization.cpp FindCrowdedShaders, cache_storage ForEachBlobName): before a
  warm-up reads the store, binary names are counted per program; a program with >= 100 cached
  versions is skipped by both warm-up paths (ReadWarmEntry, LoadPipelineStage) and its binaries,
  metas and c_ pipeline keys (all named by HashCombine(program, index)) are removed through the IO
  queue. On-demand compiles then start that program at index 0 again.
- Verified (build 694e265e, desktop): U2 run 1 removed both (383 + 1,268), 82 compute pipelines
  (was ~1,730), 60 FPS, no storm lines; cache files 6,894 -> 5,245 spv, each storm shader left with
  its 1 new version. U2 run 2: nothing removed, 5,026 keys, 8.8 s warm-up, 60 FPS. U1 and U3 runs:
  82 compute pipelines, 60 FPS, no faults. Backup of the removed files:
  analysis/backups/crowded-before-1021. Laptop package pkg/fix-21oct-7.
- Other issue findings 21 Oct: #102/#107/#94 = U1 0x64011f (CUE pretouch fix); #109 = the same crash
  in U2 (0xd667ff, fill 0xd645f0 + 0x220f as in U1); #105/#74/#29 = 8f067540 storms; #103 =
  AnkerGames repack without files.json; #104 exit 23 = guest debug exception (log needed); #77 movie
  upscale costs 3.3 ms/frame at 4K here (not the cause; open).
- 21 Oct ~13:45: laptop Q2 (U2, NVIDIA GPU, fix-21oct-6, the user playing) stopped at ~16 min on
  image_info.cpp:191 ASSERT(!props.is_block) after "SurfaceFormat: Unknown data_format=36 and
  num_format=13": a garbage T# (BC2 + Ubscaled) with a macro-tiled mode; ImageInfo's constructor
  asserts before PlausibleImage runs. UpdateSize now returns with guest_size 0 for block +
  macro-tiled (never a real texture: it always asserted), PlausibleImage drops it. Build 5cff3a16,
  deployed (backup out/gow3-probe.before-blockmacro.exe = 694e265e), pkg fix-21oct-8; desktop
  U1/U2/U3 regression 60 FPS, no faults. Q2 memory to the stop: RAM 5.1 -> 9.0 GB private, VRAM
  1.7 -> 5.6 GB, arenas 320 -> 1,984 MiB (limit 1,989 MiB, none given back yet), images 1,374 ->
  652 MiB after GC.
- 21 Oct ~14:40, proof of the garbage-T# fix: test switch GOW3_TEST_GARBAGE_TSHARP=<s>[,<n>]
  (vk_rasterizer.cpp TestGarbageTSharp, main bind path, off by default) turns the next n T#s into
  BC2 + number format 13 + a macro-tiled mode. Build without the fix (9d586046) + switch: the exact
  Q2 stop (SurfaceFormat warning, image_info.cpp:191 UpdateSize, STOP). Fixed build: dropped
  ("Ignoring a garbage T#"), but a forced T# in a depth slot then stopped in PromoteFormatToDepth
  (UNREACHABLE for Bc2UnormBlock): ImageInfo/ImageViewInfo now promote only promotable formats
  (IsPromotableToDepth) and give the rest size 0. Final build 8b74cda0 + 3,000 forced: 60 FPS for
  75 s, no stop; U1/U3 regression 60 FPS, shadows correct. Deployed (backup
  out/gow3-probe.before-depthgarbage.exe = 5cff3a16), pkg fix-21oct-9. Test exes:
  analysis/test-tools/bench/exe-garbage-old.exe, exe-garbage-fix2.exe.
- 21 Oct ~16:25, cache swap (laptop T1): no cache went to the wrong GPU (folder profiles: active +
  gpu-2 NVIDIA, gpu-3 AMD). Build 7 (no SwapCache, clears) ran at 13:23 on the NVIDIA GPU and deleted the
  780M cache, leaving two NVIDIA GPU caches; t1c restored the first by folder order. SwapCache now
  restores the newest matching cache (folder stamp) and removes older ones of the same GPU. Desktop
  test (5080 + AMD iGPU, fake older NVIDIA duplicate named gpu-0): iGPU start restored its cache,
  NVIDIA start restored the real one and removed the fake. Build f36c6f02 deployed (backup
  out/gow3-probe.before-swapnewest.exe = 8b74cda0), pkg fix-21oct-10. Cache backup:
  analysis/backups/cache-before-swaptest-1021 (13 GB). User's U3 session 14:5x-15:18 on 8b74cda0:
  ~42 min, median 60, min 57, exit 0.
- 21 Oct evening, new features (deployed 971e6a41, unreleased): surround sound (runtime_audio.c:
  the games mix their 8-channel port for the layout GetPortState reports; port reported stereo so
  only L/R had sound; dumps: told 8 -> all 8 channels active, told 6 -> L R C LFE + surrounds in
  7-8; auto = device channels; launcher Advanced -> Speakers), controller light bar
  (SDL_SetGamepadLED; colours 0000ff/202000/400000), trophies (saved per list in
  user/trophy/trophyNN.txt, unlock state reported, PS5-style popup with the pack's TROPnnn.PNG +
  chime, menu Trophies tab; label 1/2/3 = trophy01/02/03.trp, 54/53/59 trophies; names are in
  encrypted ESFM and the dump has no npbind.dat, so not shown). Tests: services-test (trophy
  logic), GOW3_TEST_TROPHY screenshots in all three games, audio dumps.

## 28 October 2026 (night): open issues pass (analysis/issues-1028/MORNING.md, TRIAGE.md)

- Treasure model invisible with upscaling (#260 #225 #209 #136): drawn into the display buffer with
  its own render-size depth; colour redirected to the output-size host display, depth not -> "pass
  not redirected" -> drawn into the hidden guest buffer. TemporalUpscaler::RedirectDepth(...,
  color_redirected) now adopts such a depth (UI phase, or display_redirect after the composite):
  EnsureUiResources + PrepareUiDepth (clear + nearest blit) at first use per frame
  (ui_display_depth, in RedirectState bit 3). GOW3_UI_DEPTH_ADOPT=0 = off. Verified U3 Treasures
  1280x720 -> 1080p FSR 3: old exe empty, new exe shows the model.
- AMD device lost with FSR/TAA (#273 #272): reproduced on the desktop AMD iGPU (RDNA2) via
  VK_LOADER_DRIVERS_DISABLE=*nv-vk64* (SELECT=*amd* also matches NVIDIA's "amd64" path): ReadInvalid
  0x3ec00000 on the first upscaled frame (2/2), none with GOW3_OBJECT_MOTION=0. Root cause: the motion
  VS built its buffer-device addresses as 64-bit sums (constant base + index * stride); AMD's
  compiler kept the low 32 bits (params at 0x3'3ec00000 read at 0x3ec00000). EmitVertexMotion now
  adds the 32-bit offset to the low half with OpIAddCarry and bitcasts U32[2] to U64; the first
  "off on 0x1002" workaround is removed. Verified AMD iGPU + RTX 5080 (U1 FSR 3, 220 s each,
  7200/7200 draws stored and matched).
- Starter (tools/windows_starter/starter.c) waits for the launcher (#267 Steam play time);
  launcher dark title bar (#215), PlayStation touchpad labels (#214).
- Deployed c151bcd0 (old = out/before-issues-1028.exe); package built.
