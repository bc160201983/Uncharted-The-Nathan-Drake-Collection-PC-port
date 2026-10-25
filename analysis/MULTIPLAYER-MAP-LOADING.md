# Multiplayer maps and retained native loading contracts

Investigation and isolated diagnostic integration, 2026-10-24. Original game
assets remain unchanged; the restored stage manifest is a separate mod overlay.
Earlier failed experiments below are retained as historical evidence.

## Current verified result: native local stage with movement

The stage-static-spawn run (owned PID26568, imagebase0xc400000) succeeded after
the parent changed cross-worker pose publication to immutable global storage,
pinned the map/wrapper, and required complete level loading before native spawn.
Player::Init returned0. Read-only snapshots establish:

| Evidence file | State | Player XYZ | Camera XYZ |
| --- | --- | --- | --- |
| stage-static-spawn-state.json | IdleAmbient | 22.698759, 1.000003, 20.606701 | 23.865629, 2.098778, 24.296110 |
| stage-static-forward.json | Move | 20.680817, 1.000000, 13.385100 | 21.831823, 2.098778, 17.079494 |
| stage-static-after-camera-jump.json | Move | 20.680817, 1.000000, 13.385100 | 17.352621, 2.098778, 15.359037 |

All three retain a valid Player handle. The initial XYZ exactly matches the
retained online-spawner-1359; the forward-input delta demonstrates actual native
movement, while camera input changes camera position without changing the
recorded player position. Stage and wrapper both report state7, keepflag40 and
readyflag80. These JSONs are in work/local-lobby/map-evidence.

Framebuffer captures are in a local capture folder (not in the repository):
UC2-stage-native-spawn.png, UC2-stage-native-forward.png and
UC2-stage-native-camera.png. The additional jump capture does not independently
prove jumping: the post-input snapshot retained the same recorded Y.

This verifies one local native player in the restored stage. It does not yet
verify menu-to-world transition, match rules, remote player replication,
combat/scoring, or Village/Warzone player placement.

## Village and Warzone: recovered PS3 online metadata

The required wrappers are absent from the PC loose assets and inspected PC
archives, and from the PS3 disc pak22.psarc. They exist in the user's downloaded
RPCS3 cache at:

`<RPCS3>/dev_hdd0/game/BCUS98123DATA2/USRDIR/build/main/pak22/`

- `1-village-multi-pak-ingame10.pak`: SHA256
  `6ab6453c70238d4424a8bec46dda8aab1c2987de8b94f6d2d9557e55a1d6b382`.
  Contains41 online-spawner and10 camera-entity records.
- `1-warzone-multi-pak-ingame10.pak`: SHA256
  `be7184ab479804680fc06c38738bc583ed2cab81f9bfccf9afac94b0e681c15a`.
  Contains35 online-spawner and9 camera-entity records.

These PS3 PAKs use big endian and32-bit pointers. They must not be loaded raw
as PS4 packages. `compare_ps3_pc_spawns.py` validates PAK/chunk bounds, resource
header magic, every selected name's native SID CRC, finite XYZ/quaternions and
quaternion norm. It compares common names from the PS3 and PC geometry ingame
packages: all248 Village and272 Warzone records have **exactly equal** XYZ and
quaternions (maximum delta0, accounting for equivalent quaternion sign).
This establishes matching coordinates/units for those asset sets; it does not
alone establish runtime level transforms or original online spawn selection.

Full pairwise evidence and source hashes: `ps3-pc-spawn-comparison.json`.
Clean extracted XYZ/quaternion catalogue: `extracted-online-spawn-catalog.json`.
The PS3 fourth position lane contains large unrelated values and is deliberately
excluded from the clean catalogue. PS3 parent field+40 is zero for all selected
records. Its exact native semantics are now independently established below.

No Village/Warzone manifest overlay was generated: their wrappers have no native
PS4 counterpart. A future controlled bridge can consume the extracted metadata
after verifying the loaded PC level's world frame, retaining the native player
factory, and validating placement/camera at runtime. This avoids fabricating a
guest Spawner or treating a PS3 PAK as ABI-compatible with PS4.

### PS3 parent/world contract and maintained extraction tool

The PS3 registration atd9a454 identifies DcGetSpawnerPosition; its descriptor
ddeee0 resolves code145650 (TOCe18380). That command calls7c7ac. Rizin and the
matching recomp function func_0007C7AC establish:

- 7c7c8 reads Spawner+40 parent. Non-null enters recursion/parent actor composition.
- 7c8cc reads root owner atSpawner+3c. The root branch adds owner+20 vector to
 position+0, then copies quaternion+10 unchanged.
- Console loadlevel baf68 tailcalls bade0; new-level initialization callsb25d0.
 b25dc clears a vector with vxor, b25e4 selects offset20, andb25ec stores the
 zero vector into Level+20. Thus ordinary initial level origin is zero.

Proof files: ps3-spawner-command-rizin.asm, ps3-spawner-transform-rizin.asm,
ps3-level-load-rizin.asm and ps3-level-origin-rizin.asm. Audited EBOOT.ELF SHA256
13b11b420508086cf5fdf3f7f99a712352b7c2dc1172228c55be00aca0c80eeb.

Maintained `tools/uc2mp_extract_spawns.py` now pins all six PAK hashes plus the
PS3 ELF hash/signatures, verifies520 exact common root transforms, and rejects
non-root selected spawners. Its output verified-online-spawn-catalog.json contains
95 pc_pose32 vectors (XYZ,w0,quaternion), exact provenance, and the runtime
requirement: loaded PC Level state7, readyflag80, keepflag40, and origin+20 zero.
It creates metadata only, never a converted PAK. Existing different outputs and
outputs inside original asset trees are rejected. Three tests cover both endian
layouts, truncated/invalid metadata and unknown executable contracts.

CLI arguments are --base (PC uroot), optional --patch (update uroot), --ps3-cache
(RPCS3 DATA2 build/main/pak22), --ps3-elf, and --out. Revalidation of the live PC
origin remains necessary before using the extracted root poses for spawning.

## Identity and tools

PS4 1.02 reconstructed `analysis/big2-ps4_Shipping/eboot.elf`, SHA-256
`09546f611636dd642fceab51469714e1a7ffd529fa3a009525659902882bad18`.
Rizin 0.9.1 portable was used for the disassembly in
`work/local-lobby/map-evidence/*-rizin.asm`; loader warnings retained alongside.
Addresses below are guest image virtual offsets, not host addresses.
Rizin ELF loader data can be unbacked; relocation targets were read independently
from the ELF dynamic relocation table. Capstone supplementary scans are named
`*-capstone.asm` and are not presented as Rizin results.

Base game: `<downloads>/Uncharted.The.Nathan.Drake.Collection/CUSA02344/uroot`.
Update: sibling `CUSA02344-patch/uroot`.
The base `u2data/build/main/dc1/ps4/net.bin` SHA-256 is
`15f79473ec9b27bd592dba8d5e26064193869e65cd1da2ad35535417ff6534e6`.

## Assets actually present

233 base/update files with multi/coop in their filename, totaling 1,396,647,011
bytes (includes both base and updated village-multi, not unique content size).
This is a bounded filename inventory, not a completeness claim. Evidence:
`asset-inventory.json`, `level-catalog.json`, `map-dependencies.json`.

| Level | Main geometry bytes | Ingame bytes | Direct declared dependencies |
|---|---:|---:|---|
| stage-multi | 801560 | 104216 | 57/57 package/actor/texture paths present |
| village-multi | 67492512 base / 67492544 update | 657752 | 65/65 present |
| warzone-multi | 80183376 | 843600 | 61/61 present |
| train-wreck-multi | 29570664 | 499832 | Two actor paths absent, see below |

Also retained: dig-multi, gompa-multi, ice-cave-multi, ruins-multi,
shambhala-multi, the-fort-multi, treasure-multi and world-multiplayer-ingame10.pak
(88528 bytes). `ss-multiplayer.bin`, `anim-net-player.bin`, `net.bin`, and many
multiplayer part modules survive. File presence does not establish successful
shader/collision/script initialization.

train-wreck-multi declares actor files `mp-wrk-crash-boxcar` and
`mp-wrk-explode-tanker`, not found among loose base/update files inspected. They
could exist in archives; archive absence has not been established.
`sniper-towers-multi-ingame` also has no matching loose package in the inventory.

## Original map/config table contracts

`net.bin` directory pairs use ID32 and pointer64. Registry ID `0x0dec97a4`
points at file offset0x3a0: count25, record pointer0xb30, stride0x38.
Map record fields established by loader641690:

- +0x00 native map key (distinct from level-name CRC)
- +0x18 pointer to wrapper name, e.g. village-multi-pak
- +0x28 pointer to {count32,+8 list pointer}; each dependency is a 16-byte
  {string pointer64, SID32, padding} record

Registry ID `0x3b9a067d` points at file0x588: count284, records0x1170,
stride0x90. Config+4 is native map key, +0x10 must be nonzero (640fc0 asserts),
+0x78 identifies an optional script resource checked by64b770.
Manager+0x1ca0 is the signed16 config INDEX, not map ID.

| Level | Original map key | Original config examples | Level-name CRC |
|---|---|---|---|
| stage-multi | 4cdd8723 | 123 | not needed here |
| village-multi | 1cb87d9c | 25..29 | dbaad5e4 |
| warzone-multi | 9fc67f05 | 30..34 | 64b8d44b |

Config variants remain unnamed: do not claim a specific native game mode until
its referenced SIDs and task behavior are resolved. Full decoded tables in
`native-map-catalog.json`, `native-map-table.json`, `native-config-table.json`.
The host lobby catalog's own mode/level IDs must not be written as guest config
indices.

Important gap: original map table wrapper names village-multi-pak and
warzone-multi-pak are neither declared level-name entries in pak22_ps4.txt nor
matching loose packages. stage-multi-pak-ingame10.pak exists, but its level-name
entry is absent. The map table still names PS3-era wrappers while actual retained
PS4 maps have different directly loadable names. Reusing the original online
loader without adapting requests can fail despite surviving geometry.

## Smallest native engine experiment

The shipping 1.02 binary retains startup options:

- `level <name>`: compare3f31c0, handler3f2960 adds name pointer at
  BigConfig1059640+3a10 and increments count+3a08.
- `player` (alias p): compare3f34ce/3f34e5, handler3f2b62 sets
  BigConfig+4818=1 (`m_autoSpawnPlayer`).
- `task <name>`: compare3f37ae, handler3f2f42 copies128 bytes to147ea20.

Candidate isolated diagnostic guest arguments: `level stage-multi player`.
Pass as separate guest args, preserve the runtime's normal content profile,
modules, and isolated user directory. This has not been runtime-tested by this
investigation. It should be described as a retained map/player diagnostic, not
as a multiplayer match. Stage is the smallest verified map pair.

Startup3f8406 checks autoSpawnPlayer then calls969320 with no arguments.
969320 derives transforms from current camera handle1114208 or falls back to
origin/identity, loads several weapon actors, obtains registry c66bbbc7, and
calls975190 with position transform, camera transform, string hero-default,
registry pointer, r8=0, and a zero stack argument. It may spawn at an unsuitable
position if the map supplies no camera; no safe spawn coordinates are inferred.
This routine must run in the initialized engine allocator/thread context.

Startup3f8bd2 ORs actor count+479c and level count+3a08. Any nonzero count skips
initial task activation. Therefore adding `task play-testing` together with
`level stage-multi` does NOT establish task startup. Without explicit levels,
task selection searches registry6e51b48f/67723261 and calls a5b020(descriptor),
with fallback a572d0/a57dc0. tasks.bin contains online -> world-multiplayer and
ss-multiplayer, and a play-testing entry; their mere presence does not establish
that native Online requirements can be bypassed.

## Low-level streaming function

Console registration56e3b7 binds loadlevel ->56e9b0(argc,argv).
56e9b0 requires argc>=2 then tailcalls:

`56ad90(RDI=&1130390, RSI=argv[1], EDX=0)` (SysV guest ABI)

Returns Level* or NULL. Looks up specification through568460, creates through
56a620, writes word[Level]=1, qword[Level+908]=spec, dword[Level+30]=1 for async
loading. Nonzero EDX invokes a synchronous loader and can spin/drain queues;
do not use synchronously on the menu callback. Streaming a map does not create
players, select a camera, hide the frontend, or run match rules.

Important existing-level exception:56ae45..56ae4c returns an existing Level
directly via56aec7, even when EDX=1. Callers requiring completion must check
state+30 and use the verified56ec70(Level*) completion path if it is not7,
then check state7 and flags+18 &80. The GameInit call56bf60 at3f88df only
enqueues CLI level requests; it is NOT a loading barrier. Delaying spawn until
after that call alone does not establish map readiness.

## Original Online task pipeline and blockers

Factory5ea060 allocates0x158 using the guest TLS allocator and installs
vtable100a810. LoadLevels transition5ea4d0 is vtable slot+158 and registers a
state object via factory5ea460, whose vtable is100ac00. Relocations prove:

- state enter5f2e50 ->640fc0(manager113b760, ownerTask, 0)
- state update5f2dc0 ->64b770(manager, ownerTask)
- state leave5f2e30 updates loading flags

640fc0 immediately traverses manager pools+1ba0/+1bd0 and+1c10/+1c40;
zero-filled replacement memory is not a valid manager. It resets pools, calls
641690(manager,1,0,0), then3f4ca0, resets match state, and requires a valid
nonzero config+10. 641690 uses original config/map tables, requests wrapper
and level names through56c0e0, and actor dependencies through56d170.

64b770 dereferences ownerTask+80 then descriptor+88, reads descriptor+28,
checks streaming through56ce50 and script state8. It also checks optional
config script+78. After it succeeds,5f2dc0 still needs a real local player at
manager+8 with exclusion byte+26c clear; otherwise it transitions to failure.
Thus a native host-model token cannot be passed as the task/manager/player.

A complete replacement match bridge still needs verified: frontend-to-world
transition, real engine player creation at a valid map spawn, camera ownership,
controls/input ownership, script/rule startup, and clean return/teardown. The
startup diagnostic above can validate assets and rendering without pretending
these contracts are already solved.

## Follow-up readiness and diagnostic coverage

The reusable read-only `work/local-lobby/map-evidence/audit_maps.py` validates
loose package paths and the seven actors referenced by native autoSpawnPlayer.
`stage-hero-audit.json`: 64 direct checks, zero missing; hero-default.pak is
8,370,960 bytes. This excludes recursive actor dependencies and archive checks.

Original ready predicate56ce50 checks every requested level for state
`dword[Level+30]==7` and flag `byte[Level+18]&0x80`; its request-list counts
are manager+5a98 and+5aa8. Empty request lists can return success, so calling
56ce50 after only direct56ad90 does not prove that particular map is ready.
Track the returned Level* and validate its lifetime before reading fields.

Unload console callback56e8e0(argc,argv) hashes argv[1], looks up an existing
Level, and calls5704c0(Level*) if found. This is evidence for level cleanup,
not complete actor/camera/frontend teardown; it was not executed here.

The startup path creates a camera process through9b5c20 using type SIDe2fb13e6
and context from game object+c80 (3f82b4..3f8345), then stores its handle in
1114208 before autoSpawnPlayer. This makes the existing startup path preferable
to calling969320 from an arbitrary menu frame. A valid spawn location remains
unverified.

Parent's isolated stage diagnostic launch stopped at Vulkan surface setup
(`vk_swapchain.cpp:251`, ErrorUnknown) before guest initialization. This is
neither a map-loading failure nor a map-loading success. Existing confirmed
lobby process was preserved. No runtime verdict is recorded for stage or hero.

## Follow-up: offscreen stage and read-only world snapshot

After introducing opt-in real Vulkan offscreen rendering (see
`WSI-OFFSCREEN-DIAGNOSTIC.md`), the parent ran the native guest with
`level stage-multi player`. The first capture was gray while loading; the second
capture around 90 seconds showed real stage grid/block geometry and shadows.
This confirms retained stage rendering, not a complete multiplayer match.
A visible Drake avatar and player control remain unverified.

`work/local-lobby/map-evidence/probe_world.py` provides a bounded read-only
Windows process snapshot. It takes an explicitly owned PID and the guest image
base from that process's log, verifies the unmodified 1.02 autoSpawnPlayer
prologue, and uses ReadProcessMemory only (no guest calls/writes/suspension).

- Camera handle RVA `0x1114208`, hero handle RVA `0x115bd20`.
- Each handle is a slot pointer (u64) and generation (u32). Resolve its object
  through the slot only when both are nonzero, and require the object's u32 at
  +0x44 to equal the generation. The probe rereads these after the snapshot.
- Camera position vector is +0x370 and quaternion is +0x380, verified by reads
  in `0x969320`. The native routine offsets its player position by literal
  `(0,-1,0,-1)` and computes a separate camera transform. These are existing
  engine calculations, not proposed spawn coordinates.
- `0x975190` constructs process type SID `0xa5a63e9b` through `0x9b5c20`, then
  publishes the real hero handle at `0x975620..0x975632`.
- Level-manager RVA `0x1130390` owns a bucket pointer at +0x10 and signed bucket
  count at +0x4c. Bucket lookup hashes the four little-endian native SID bytes
  using FNV-1a. Nodes contain next +0, key +0x10, Level* +0x18; the sentinel is
  buckets[count]. The probe limits traversal to 64 nodes and checks stability.
  Stage's native SID is `0x3fc7a4ea`. Readiness is the verified state/flag pair
  above, not inferred from an empty global request list.

An additional relevant native fallback: `0x97533f..0x975427` searches the actor
registry at qword[RVA0x1241418+0x38], 350 slots of size0x2c0, requiring type12 at
+0x20 and a skin SID at +0x144 or +0xc0. If the requested skin is absent, it loads
and uses `proto-mesh`, even though `hero-default.pak` exists on disk. None of
the stage/village/warzone level actor lists directly requests hero-default;
common cache dependencies may still provide it. The probe reports current
hero-default/proto-mesh cache entries and the requested player-skin string at
RVA0x1164458. Current cache state alone cannot prove what was loaded at the
moment of spawn.

Evidence: `hero-create-rizin.asm`, `hero-publish-rizin.asm`, and source comments
in the snapshot tool. Live snapshots are best-effort; the engine is not paused.

### Hero preload ordering (static verification)

The native `extra <actor>` argument queues names at BigConfig+0x47c8 and count
+0x47c0. Its synchronous load loop is `0x3f86f0..0x3f8727`, **after** the call
to autoSpawnPlayer at `0x3f8410`. The `actor <name>` loop likewise runs later at
`0x3f8690..0x3f86c7`, additionally creating an actor-viewer process earlier.
Neither argument alone prevents the initial proto-mesh fallback.

The retained preload sequence is independently visible in both those loops
and six weapon loads inside autoSpawnPlayer:

1. `0x56c200(&manager_RVA1130390, guest_name_pointer, 0)` returns Level*.
2. Set bit0x40 in its flags word at+0x18.
3. If state at+0x30 is not7, call `0x56ec70(Level*)` (the native synchronous
   load/drain path).
4. Clear bit0x20 in flags word+0x18.

Once a runtime snapshot confirms the skin-cache fallback, a possible scoped
init-thread bridge is to perform this exact sequence for the retained
`hero-default` literal (RVA0xe9466e) immediately before the existing call to
`0x969320`. This is a proposed application of verified static contracts, not
runtime validation of that bridge. It must remain opt-in and must not run the
synchronous loader from a menu callback. Evidence: `extra-actor-load-rizin.asm`
and `hero-create-rizin.asm`.

The parent's `stage-world-before.json` runtime snapshot subsequently confirmed:
stage state7/ready, valid camera at position0, hero handle generation3 with a
nonzero slot but NULL resolved object, and proto-mesh cached while hero-default
was absent. This establishes a concrete missing-hero state without assuming
that preloading alone will solve camera or placement.

`gow3_pc/src/uc2mp_spawn_bridge.h` now implements that opt-in preload bridge.
It requires exact `UNCHARTED_UC2MP_PRELOAD_HERO=1` plus a guest command line with
one supported `level` and `player`. The parent launcher must set the variable
only for its `--map-test` action. Include the header after `allocate_near_image`
in probe.c; call `install_uc2mp_spawn_bridge(segments,ns,size,guest_argc,guest_argv)`
after patches and before guest execution/protection, when GPU execution is
enabled. The header changes only the rel32 destination of the existing init
CALL, preserving its conditional and return point. Its near trampoline jumps
to a SysV wrapper without clobbering registers or changing stack alignment.

Signatures cover the conditional/call site, loader, synchronous loader,
autoSpawnPlayer and retained name literal, with executable/readable/writable
segment checks. No mutation occurs if any check fails. A missing/unmapped
Level or unsuccessful readiness predicate skips autoSpawn with an explicit
error. Successful preload retains the native flags operations and calls the
unmodified original autoSpawn routine.

`test_uc2mp_spawn_bridge.c` passed with `-Wall -Wextra -Werror`: absent env,
normal-menu exclusion, invalid mapping, wrong signature, native-boundary
sequencing, NULL/unmapped/unready failures, and a positive installer check
against the actual ELF1.02 (no guest execution). Runtime integration and avatar
verification remain the parent's next test.

### Hero lifetime investigation (2026-10-24)

The `stage-hero` runtime reached hero-default Level state7/flags0x2c9, and its
actor cache contained hero-default. The later snapshot still resolved the
published hero handle to a NULL object, while the stage geometry rendered.
Preloading the skin alone therefore does not establish a playable actor.

The opt-in bridge now logs the hero handle immediately before and after the
original call, including slot, object, generation, object generation, type,
vtable, process flags and the two initialization callback addresses. The ELF
global is bounded by the install-time PT_LOAD RW contract and image size;
`runtime_memory_is_mapped` only tracks runtime heap mappings and cannot validate
ELF globals. The first lifecycle run reported `readable=0` for this reason;
that run is inconclusive, and the guard has been corrected. Unit tests now
model this exact distinction, including stale generations and NULL slots.

A read-only registry snapshot of the owned `stage-hero-lifecycle` instance
(PID17580, guest base0xc400000) established:

- Type registry RVA0x145ca68 is initialized. Bucket = type SID modulo257;
  head at registry+8+16*bucket, linked node key+0x10.
- SID0xa5a63e9b names `Player`, registered allocation budget0x80000,
  constructor RVA0x969610. The constructor initializes vtable RVA0x1010980.
- Vtable+0x20 is Player initialization RVA0x96bc80; +0x28 is0x474bc0;
  +0x30 is0x71df30; +0x70 is0x9967f0.
- Player initialization **also publishes the global hero handle** at0x96bd00,
  before calling base initialization0x720b30. That calls0x473f80, then0x991550.
  Positive status returns propagate to the process factory.

`NewProcess_Internal` RVA0x9b5c20 calls the registered constructor, then vtable
+0x70 with its parent context, then vtable+0x20 with the descriptor. If its
return is nonpositive it calls+0x28, and a nonpositive result can run+0x30.
It returns NULL for processes marked with flagsword+0x34 &6. Creation failure
can thus leave a published stale handle, without a successful975190 return.
Conversely, a valid immediate snapshot followed by NULL would prove later
lifecycle destruction. No callbacks have been replaced or forced to succeed.

Static ordering is also relevant: autoSpawn at3f8410 precedes common actor loads
(dummy-camera-object/proto/weapons at3f8758 onward) and the requested stage
load. The boot log has the same ordering. Missing initialization dependencies
or subsequent world reset remain hypotheses until the immediate snapshot or
specific return-status diagnostics identifies the actual path.

Evidence: hero-registry-live.json, hero-vtable-live.json, process-factory-rizin.asm,
process-factory2-rizin.asm, hero-init-capstone.asm, actor-init-capstone.asm,
base-actor-init-capstone.asm, process-actor-init-capstone.asm, and
spawn-bridge-tests.log. `disasm.py` provides bounded Capstone extraction of the
same ELF; Rizin remains the independent annotated-disassembly source.

The corrected immediate snapshot (`stage-hero-immediate`) resolves a non-NULL
Player object, but its generation is already0 versus the published generation3,
and its flags are0xa657. This is an invalid/dying process, not a playable hero.
RVA0x9be450 establishes the exact meaning: it formats a failure message, calls
vtable+0xb0, writes object+0x44=0 at9be537, ORs flags+0x34 with0x8002, then
calls vtable+0xc8 with the formatted message. The observed flags include both
bits. The offset+0x44 was correct; changing the generation would hide failure.

The bridge now installs passive Player vtable callbacks before image protection:
+0x20 (96bc80) logs the exact init result; +0xc8 (99ba60) logs the failure text.
Their original relocated pointers and entry signatures are verified. Both
always call the originals, preserving results/arguments. Logging is active
only in a thread-local scope around the original autoSpawn; no runtime table
writes or protection changes occur. The real-ELF test includes the RELRO
segment and applies the two actual R_X86_64_RELATIVE relocations from the ELF
before testing this installation, as the normal loader does. Pass-through
behavior with logging inactive/active is tested. No native instructions run
inside these tests.

Potential status decoding, independently recovered from B396e0's error table:
0x0cf60009 = "Skeleton Not Loaded" (table RVA1045280, entry9 points to EAE70D).
The base actor initializer991550 returns this code when its required skeleton
SID has no loaded skeleton entry. This is a candidate failure until the new
runtime trace produces its actual status/message; no preload was added based
on this hypothesis.

### Verified proto prerequisite experiment

The UI investigation independently matched hero-default's foreground skeleton
SID0x27e57f43 (FOREGROUND_SECTION_2 header0x242db0, field+0x28) to proto.pak's
JOINT_HIERARCHY SID0x27e57f43 (header0x784b40, field+0x50). These fields match
993b20's foreground fallback and991550's skeleton lookup. GameInit loads proto
after the early autoSpawn call. This provides concrete dependency/ordering
evidence without relying on the still-unobserved error status.

The opt-in bridge now preloads `proto` then `hero-default` using the same
verified synchronous Level contract for each. The guest literal `proto` at
RVA0xeb05cd is signature-checked. If either Level is invalid or not ready, the
original autoSpawn is skipped with its package name in the diagnostic. Tests
cover proto completion, proto failure stopping before hero, correct load order,
hero failure, and all actual-image signatures. This is still a guarded runtime
experiment: it does not establish that the Player survives or is controllable.

The first vtable-trace run did not print callback logs despite the installed
pointer appearing in the dying object's vtable. This does not prove a separate
thread or devirtualization. Bounded first-eight callback entry logs, atomic
counters and TLS addresses now distinguish these possibilities; original
results and arguments remain unmodified. Logs before/after autoSpawn also
include those counters.

### Runtime cause confirmed and hero now survives

`stage-callback-counts` supplied the previously missing runtime evidence:
Player::Init executed with a different host TLS instance and scope0, returned
0x0cf60009, and its failure callback reported `Skeleton Not Loaded`. Callback
counters were1/1. The earlier TLS-filtered absence of logs was not evidence
that the native calls were bypassed. Guest initialization can run on another
worker thread; the bounded, unfiltered callback diagnostics captured it.

With the verified proto-before-hero preload, `stage-proto-hero` returned init0,
recorded zero failures, and retained a valid Player handle generation3 for more
than90seconds. Its camera moved away from the untouched origin. This validates
creation and survival, but not yet spawn placement, avatar control, or a match.

`probe_world.py` now reads the Player relative transform from+0x110/+0x120 and
cached parent transform from+0xf0/+0x100, with an additional994080 signature
check. The native descriptor transform setter994080..994206 proves those
fields: it resolves/caches the parent, transforms the supplied world transform
into relative space, then stores those two vectors. The fourth float is raw
engine data, not assumed to be a homogeneous coordinate. A double read records
stability; no guest function is called and no memory is written.

`stage-proto-hero-transform.json` captured valid Player generation3 at relative
XYZ(0.14649,-40.13015,0.00593), with cached parent at origin and identity
quaternion. Camera XYZ was(0,-3.46809,-3.96342). This sample alone cannot prove
input response or correct placement. Compare the same fields before/after
controlled input while retaining handle generation and parent-transform checks.

### Retained spawn records, no invented coordinates

`audit_spawn_records.py` validates the package/chunk table and SPAWNER_GROUP
header magic, follows its bounded pointer array and extracts matching records.
Results and complete file hashes are in `native-spawn-records.json`.

The base `stage-multi-pak-ingame10.pak` survives (53072bytes), although no
`stage-multi-pak` level-name entry was found in either manifest. Its spawn group
has85records, including37 online-spawner and2 camera-entity entries. Example:
`online-spawner-1359`, fileoffset0x3be0, record first vector
(22.698759,1.000003,20.606701,0), second vector approximately
(0,0.707107,0,0.707107). Stage's geometry ingame package instead contains a
BSPAWNER_GROUP. The village ingame package has248 SPAWNER_GROUP records with
no online-spawner/camera-entity names in this selection.

These are retained asset transforms, not invented positions. Runtime selection
of these entries and activation of the manifest-absent stage wrapper remain
unverified; the audit does not patch positions or instantiate any spawner.

### Active Player state and loaded spawner inventory

`probe_world.py` now reads Player flags+34, TimeContext pointer+70 (frame+0c,
raw/float dt+14), StateMachine pointer+50 (current+0,next+8), Skeleton+188,
AnimControl+190 (raw u64+8/+130/+148), and current state descriptor+10
(class string+0, state string+8, constructor+10, SID+18). Each diagnostic field
has isolated bounded-read errors. Pointer dereferences require an exact readable
sample; outer Player generation is checked again. No process suspension/writes.
The two fake-reader tests cover missing fields and invalid/null subobjects.

`stage-proto-state.json` confirms valid generation3, flags6ab1, dt0.01697,
readable skeleton and matching AnimControl skeleton, and local Y=-40.317.
`village-state-name.json` confirms the same valid Player but state Player/Death
(descriptor RVA14e17d8, constructor RVA79c5a0, SID4f875693), local Y=-127.357.
These observations establish that initialization succeeded but the player died;
they do not establish a playable local match.

`probe_spawners.py` follows the exact native iterator37ffd0/380120:
manager+0 Level array, stridea60, bitmap180 bits at+80, eligible when state7,
flagsword+18 &200, kindword+0 &1. Lookup56c7b0 then reads Level+9a0 group,
sorted pair array group+18, count+20, stride10, SID+0 and Spawner pointer+8.
Level name+34 is established by constructor56ea40. All reads are bounded and
inventory count capped; the script never invokes guest code.

`stage-proto-state-spawners.json` has75 active levels and only one spawner:
stage-multi -> monkeybars-1 (SIDb9c7036b). The wrapper stage-multi-pak is absent
from the LevelManager (`stage-wrapper-status.json`). `village-spawner-names.json`
has248 loaded records, all decor/FX/traversal/etc; no online spawn or camera
entry. Do not use arbitrary decorative object positions as player spawn points.

### Verified manifest route for the surviving stage wrapper

The native manifest parser reads `%s/build/main/pak%d_ps4.txt` (stringe86aa6,
setup569241..569266). Its level-name parser starts569418; package parser569928
uses `package %s`. At569949..56997a it compares the last6 characters to `ingame`
and appends decimal10 with `%s%u`. Evidence: `wrapper-parser-rizin.asm`.

Therefore a separate reversible manifest overlay can declare:

```
level-name stage-multi-pak
level-id stage-multi-pak
package stage-multi-pak-ingame
```

This resolves the existing stage-multi-pak-ingame10.pak. Use the level-loading
route56ad90/56c0e0 to create a type1 Level; actor-loading56c200 sets type2 and is
not the equivalent wrapper contract. Keeping the wrapper separate avoids two
SPAWNER_GROUP resources competing for one Level+9a0 pointer. The isolated overlay
is now generated by `tools/uc2mp_stage_wrapper.py` under instance
stage-wrapper-test/menu-overlay. It selects the effective patch manifest, retains
its original bytes, appends exactly one block, rejects duplicate/conflicting
declarations and output conflicts, and records source/output/package SHA256.
Two executions yielded identical output; three unit tests pass. Original game
files are unchanged. Wrapper runtime loading and additional actor/script
dependencies remain unverified until that isolated load is observed.

When a real online-spawner is loaded, native4bdfe0(out32,Spawner*) is the verified
world-transform resolver. Its root branch reads owner=Spawner+58, adds owner+20
translation to Spawner+0 position, and copies Spawner+10 quaternion. Parent+60
requires recursive parent/process composition. Supply that resolved transform
through existing975190 arguments; do not directly write actor+110/+120.

### Wrapper load and resource lifetime follow-up

Parent runtime stage-wrapper-trace confirms wrapper state7, flags299, kind1,
and both retained poses resolved by4bdfe0. The following Player::Init asserted
in MapToRagdoll, so this remains a failed diagnostic, not a playable match.
Host TLS locations changed across native loader/worker continuations; the
parent is replacing TLS pose publication with an immutable atomic single-use
publication. This is an experiment, not yet the demonstrated assertion cause.

`audit_stage_resource_headers.py` validates both package chunks and tagged
resource headers. The wrapper has TAG_INT, SPLINE_TABLE, FEATURE_DB_2,
LEVEL_INFO_1, SPAWNER_GROUP; it has no identified JOINT_HIERARCHY or Havok
resource. Its chunks are41439 and128bytes. stage-multi.pak contains the actual
COLLISION_DATA_HAVOK_BG; it was already present in prior geometry tests. This
does not prove an indirect registry/lifetime interaction is impossible.

Native keep flag40 is materially relevant:56b6a0 skips the Level unload
selection (5704c0) when set, and56e74c skips refcount decrement/unload when set.
GameInit3f8870 sets it after synchronous native level load. A diagnostic wrapper
outside normal requested level lists should preserve it while its spawner and
resources remain in use. This pinning changes lifetime, not skeleton contents.

## Black scenery on multiplayer maps (2026-10-24, open)

Seen on Warzone and Village: floors and walls of the fixed scenery render black outside
direct light; props (books, rubble, shelves) and the hero are lit normally. The surfaces
exist: the hero stands on the black floor, and a window's light patch shows the floor tiles
with their shadows (offscreen capture, uc2mp_frame_capture.py). Only the ambient/indirect
term of the background is missing.

Ruled out, each by an offscreen capture or a data check:
- Missing companion levels: net.bin map records +0x28 list them (warzone-multi-pak =
  sky-multi-warzone + warzone-multi); they are now loaded (uc2mp_map_levels.h). No change.
- Visibility culling: the same pieces stay black from every position (Tony).
- Bluepoint cubemaps (bpdata1/<level>.json): a sky cubemap at intensity 4 and a local
  indoor cubemap on the spawn point change nothing on Warzone; war-1-start keeps its
  ambient with an empty cubemap list. They only drive reflections.
- Lightmap textures (lightmaps/<level>/indirect.rgbmax*.tga): war-1-start has none and is lit.
- Shader set: warzone-multi.pak declares g_bpAmbientParams like the campaign levels.
- Resource types: warzone-multi and war-1/2/3 paks have the same lighting-related types.
- Launch path: war-1-start started the same way ("level war-1-start player") is fully lit.

- Light probes: warzone-multi has a probe tree (1384 probes, collected by 0xd02f60) like
  war-1-start (575). Render settings (pointer image+0x104aea8; +0x8c0 basis3-in-game-scalar
  = 1.0) are identical between the two runs.

CAUSE: interiors and shaded scenery are lit by per-level indirect lightmaps
(indirect.rgbmax.tga + indirect.rgbmaxdelta.tga). war-3-inside (campaign interior) carries
them on PS3 and PS4; war-1-start (exterior street) has none on either. The PS3
warzone-multi.pak (pak22.psarc entry 750) carries
lightmap0/art/levels/warzone/lighting/nova/warzone-multi.geom.atgi/indirect.rgbmax(delta).tga,
but the Collection's warzone-multi.pak has no lightmap at all. Bluepoint kept the lightmaps
of village-multi, gompa-multi, ruins-multi, shambhala-multi and the-fort-multi, not Warzone's.
FIXED (tools/uc2mp_lightmaps.py, run by the launcher with --ps3-pak22, remembered in
work/local-lobby/ps3-pak22.txt): warzone-multi's TEXTURE_TABLE entries 4 and 5 are the
placeholders default-lightmap0.tga/7CD7398D61B89710 (BC3) and default-lightmap1.tga/
84D25E61B2920A02 (BC1), 32x32. war-1-start uses the same ones, harmlessly: its PS3 pak has no
lightmap. The overlay pak appends the PS3 indirect.rgbmax/rgbmaxdelta (2048x2048, 12 mips),
tiled, to the texture block and points both descriptors at them (+0x08 offset, +0x10 size,
+0x24 mips, +0x28/+0x2c size; header +0x20 texture block size). Offscreen capture: the room
at spawn 0 is lit. Survey of every multiplayer pak: only warzone-multi lost its lightmaps.
Village's black temple top is something else (open).

PS4 pak, as used here: header and fixups after alphaZomega's nd_pak.bt
(github.com/alphazolam/nd_pak.bt); VRAM descriptor (pointed to by TEXTURE_TABLE +0x40+8*i):
+0x08 offset in the texture block after the last page, +0x10 size, +0x1c DXGI format, +0x24
mips, +0x28 width, +0x2c height, +0x38 path. Block-compressed textures are tiled: 8x8-block
tiles in row-major order, Morton (x0 y0 x1 y1 x2 y2) inside a tile, mips under 8x8 blocks
padded to one tile. Village's PS3 and PS4 lightmaps decode to the same image (PS4 re-encoded).

## Every original map (2026-10-24)

The lobby offers the 8 original multiplayer maps the Collection kept: village, warzone,
gompa, ice-cave, shambhala, the-fort, train-wreck and treasure (-multi). Their online
spawners come from the PS3 wrappers 1-<map>-pak-ingame10.pak (RPCS3 DATA2 cache), checked
against the PS3 disc <map>-ingame10.pak (pak22.psarc; the cache holds patched variants):
every PC record matches the disc coordinates exactly. Companion levels follow net.bin's
competitive records (uc2mp_map_levels.h); the-fort's sky/wide levels are not in the
Collection and it loads alone. Offscreen map test: all 8 load with the hero spawned.
ruins-multi is left out: its scenery came with PS3 DLC (ruins-multi-wide) and the map renders
empty; facility, museum and highrise (DLC) are not in the Collection at all.

Treasure (treasure-multi) interior floors rendered black too, but it has no lightmap on PS3
either: PS3 and PS4 both point it at default-lightmap0/1, whose decoded content is identical
(RGBM white with alpha 0: no indirect light; lightmap1 black). The PS3 lit those floors another
way (presumably baked vertex lighting) that the PS4 renderer does not reproduce. The launcher
writes an overlay copy whose default-lightmap0 carries a uniform low light (alpha 24,
tools/uc2mp_lightmaps.py build_neutral): offscreen captures at spawns 0 and 12 show the floors.
An approximation, not the original baked lighting.
