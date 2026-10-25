# Retained remote avatar: NetCharacter (PS4 1.02)

Read-only investigation, 2026-10-24. **The retained remote actor is
`NetCharacter`, not a second local Player.** Its original creation path and
its PS3 equivalent are present. No actor was created during this audit; no
runtime, networking, or game binary was changed. Static findings below do
not prove that this actor can yet initialize in the current stage session.

## Artifact and evidence

- PS4 `analysis/big2-ps4_Shipping/eboot.elf`, SHA256
  `09546f611636dd642fceab51469714e1a7ffd529fa3a009525659902882bad18`.
  Addresses in this report are RVAs for **1.02**, not the older 1.00 audit.
  Relevant mapped bytes have file offset = RVA + `0x4000`.
- PS3 `<ps3recomp>/UC2MP/game/BCUS98123/EBOOT.ELF`,
  SHA256 `7ad3b270aac6ec920064eccc12d1a35ebb4b99806ca22b148367e71f8ddb5e09`.
  Original disassembly and lifted C++ are cross-platform references, not PC
  function signatures or replacement object layouts.
- Portable Rizin 0.9.1 bounded `pD` extracts are under `work/rizin-mp/`:
  `remote-avatar-create-rizin.asm`, `remote-avatar-constructor-rizin.asm`,
  `remote-avatar-registration2-rizin.asm`, `remote-avatar-init-rizin.asm`,
  `remote-avatar-components-rizin.asm`, `remote-avatar-update-rizin.asm`.
  Full bounded Capstone extracts complement these:
  `remote-avatar-lifecycle-capstone.asm`,
  `remote-avatar-controller-capstone.asm`.
- `remote-avatar-vtable.json` resolves original ELF RELA records for the
  vtable. `remote-avatar-component-vtables.json` resolves the small
  component tables. The extraction matches `(r_offset, r_info=8)` and reads
  the relocation addend; zero-filled on-disk pointer fields are not treated
  as actual null methods.
- `remote-avatar-ps3-spawn.asm` and `remote-avatar-ps3-sid-refs.asm` preserve
  relevant PS3 evidence. Broad string searches were only locators; data
  bytes reported as jumps by Rizin `/r` are not code evidence.

## Original creation route

| RVA | Observed contract |
| --- | --- |
| `6636b0` | SysV `bool SpawnRemote(tracker*, u32 spawnSID, int offsetIndex)`; return in AL |
| `662fa0` | Fills 32-byte position/quaternion pose using a real level spawner; false on failure |
| `663a10` | Chooses local `663280` versus remote `6636b0` using the local tracker ID and manager flag |
| `663ad0` | Local respawn scheduling versus remote spawn with explicit offset |
| `9b5c20` | Original process factory; `(InitDesc*, errorOutOrNull)` |
| `9be760` | Original process destruction used for the previous tracker-owned actor |

`6636b0` reads tracker ID at `+120`, team at `+134`, skin index at `+138`.
It resolves skin through table SID `5a4731d2` (record stride `30`, string at
`+8`), with retained fallback strings `proto-mesh` / `hero-default`.
It calls `56a980(global1130390, skin)` and stores the result at tracker+290.

It builds an actual InitDesc on its own stack and calls the original factory:

| InitDesc offset | Value observed |
| --- | --- |
| `+00` | Type SID `f2c86c22` |
| `+08` | Context `[ProcessManager115f1a0 + c60]` |
| `+10` | Pointer to 32-byte position/quaternion pose |
| `+18` | Pointer to `{u32 trackerID, u32 team}` |
| `+48` | Skin string |
| other fields | Zero in this caller |

The pose helper accesses current level via `644130(global113b760)` and
level+9a0's sorted spawner catalog. Entries are 16 bytes (SID at+0,
spawner pointer at+8). `4bdfe0` supplies the native pose; offset indices1–4
apply an original rotated local offset. This is not an arbitrary world
position API, and a missing spawner is asserted by the original code.

After successful factory creation the old valid tracker process is destroyed,
and the new handle is saved at tracker `+10` (slot) / `+18` (generation),
from object `+38` / `+44`. Team/co-op checks may change actor+150 to `0xb`.
The actor's world center is copied into tracker+0. The wrapper does not
write global local hero `115bd20` and does not call local assignment
`975620`. This is a strong static distinction; indirect factory and virtual
side effects still require a runtime invariant check.

## Type registration, initialization, animation

The game CRC implementation in `tools/uc2mp_menu.py::sid` gives:

- `sid("NetCharacter") = f2c86c22`.
- `sid("anim-net-player") = 4ec2d815`.

Registration at `607540` hashes the literal `NetCharacter` (e88304),
stores constructor `6063c0` in registry node `14cee28` (`+20`), and records
process allocation budget `20000` at node+18. The constructor allocates the
actual `2f10`-byte object from the current native TLS arena and installs
vtable `100af70`. These two sizes have different purposes; do not allocate
a `20000`-byte fake object and assume it is initialized.

Key vtable offsets:

| Offset | RVA | Observed role |
| --- | --- | --- |
| `+10` | `5fb570` | Destructor, including native owned components |
| `+20` | `5fb7b0` | NetCharacter Init |
| `+28` | `474bc0` | Inherited post-init |
| `+30` | `607710` | Go, dispatches vtable+308 |
| `+38` | `602050` | Event handler |
| `+d8` | `9938f0` | Native AnimControl construction |
| `+f8` | `6016b0` | Binds `anim-net-player` through `3923e0` |
| `+100` | `5fe4b0` | Animation/update path consuming native sample data |
| `+108` | `5fdef0` | Pose/animation post-processing; not a simple pose setter |
| `+170` | `607700` | Returns skin string at actor+2c40 |
| `+178` | `993b20` | Shared skin/model lookup |
| `+308` | `605f30` | Schedules native `Idle` state descriptor `14ced88` |

Init stores the descriptor skin at actor+2c40 and calls base initializer
`473f80`; positive return propagates. Descriptor+18's two words become
actor+308 (network player ID) and actor+30c (team). The original factory
calls Init/post-init/Go and owns its TLS process heap. Calling the ctor or
Init directly would bypass required process bookkeeping.

**Required tracker dependency:** at `5fd79c..5fd7dc`, Init searches all ten
native tracker records under manager `113b760` by actor+308. Records start
at manager+10, stride2c0; their IDs are at manager+130+index*2c0.
At `5fd829..5fd843`, it returns `0cf10003` if that tracker, actor+1f0,
or the selected combat-settings lookup is missing. It also consumes
tracker+298 and further loadout fields. A nonzero invented actor ID with
no valid initialized tracker therefore cannot produce a successful actor.

Mode table `3b9a067d` and manager+1ca0 select combat settings; default SID
`1ad223b4` is used when no valid mode record provides its+64 setting.
Other script lookups and initialized health/equipment components remain
required. Same skin/skeleton/package prerequisites as the local actor must
be satisfied, while the animation graph is explicitly different.

`642370` / `AddOrUpdateNetPlayer` is not a safe stand-alone actor constructor:
it consumes retained session/member layouts and includes missing-platform
name inputs before populating tracker state. Do not invoke it with a
fabricated PS4 session object to satisfy this dependency.

## Transform and controller ownership

The generic actor transform is a generation-checked frame handle at
actor+d8/e0; actor+f0 and+100 are cached position and quaternion. Existing
native code refreshes these from the frame. Writing only the cache is not
a supported movement operation. Actor+110/+120 add local pose offsets.

Native NetCharacter owns separate components:

- actor+2c88 is constructed by `5f7ed0(actor)`, uses an original owner
  handle (+8 slot / +10 generation), and vtable `100ad20`. The retained
  assertion in its owner accessor names `ss-animate.cpp::GetOwner`.
- actor+2c98 is constructed by `5fb150`, then initialized through its
  vtable+118 with this NetCharacter as owner.
- actor+2c90 uses vtable `103fbb0` and stores its owner via `5f9ba0`.
  Its+10 method `5f9900` is **PlayDeathAnimation**, explicitly named by a
  native assertion. It accepts a pose and animation SID, but it is not a
  general remote movement setter. The pose-looking event path at602282
  must not be repurposed for locomotion.

The retained native sample buffer begins at actor+310. `605070` calls
`5f48e0(actor+310, actor)` from a native state update. `5fe4b0` also calls
`5f4240(actor+310, output)` before consuming samples for animation.
Both routines require sample count (buffer+68) >=3, inspect timestamp fields
around buffer+74/+104, use records of stride90, and reference playback time
buffer+960. Thus even a successfully created actor needs a valid update
contract to move/animate as a remote player. Exact sample insertion,
timestamp units, interpolation and ownership are **not yet established**.
Do not populate that buffer with guessed structs or raw network packets.

The audited creation/Init/state excerpts do not assign this actor the local
camera or pad. This is not a whole-program proof that all local globals
remain unchanged: verify local hero pointer/generation, camera, and input
ownership before and after the first runtime creation.

## PS3 corroboration

PS3 `func_004CA7F0` (`src/recomp/ppu_recomp_002.cpp:188503`) is the direct
structural counterpart of PS4 `6636b0`, not just a string match:

- Loads tracker ID+118, team+12c and skin+130 (PS3 layout differs).
- Builds the same NetCharacter SID at4ca8a0/4ca8b0 and descriptor on stack.
- Calls pose helper4c94d8 at4ca91c, factory706a78 at4ca970.
- Destroys the old actor through707e00 and stores the new handle in the
  tracker+10/+14; applies the same co-op/team flag branch.

This confirms the intended role of the retained PS4 route. It does not
authorize transplanting PS3 32-bit pointer layouts into the PS4 runtime.

## Proposed next bounded runtime contract

1. Observe an actually initialized manager tracker slot, its ownership,
   reset/registration lifecycle and required loadout values. Keep the
   native local tracker pointer and ID unchanged. Determine whether the
   existing native initializer can populate a distinct remote slot without
   old network/session APIs; this audit has not completed that proof.
2. Create one remote process through `9b5c20`, using the verified InitDesc
   layout, existing stage context and pose, with an ID matched to that real
   tracker. Alternatively use6636b0 only after its actual spawnSID/catalog
   and local tracker/mode dependencies are verified. No injected vtables,
   state-machine objects, or second call to local Player factory975190.
3. Capture Init return, process type/handle/generation, native Idle state,
   skin/skeleton/AnimControl validity, world transform and local hero/camera
   invariants. Keep it opt-in, one actor, and use native destruction on
   unload/leave. First success criterion is a second stable visible actor;
   that is not yet movement replication or gameplay.
4. Separately trace the native sample insertion callers and timing contract
   before connecting any transport state to this actor. Preserve one engine
   thread owner and publish only bounded validated data from the transport.

Open questions are native tracker population/loadout, complete sample
insertion and interpolation semantics, and behavior of the native actor in
the current single-player stage lifecycle. These remain real prerequisites,
not assumptions hidden behind a successful factory return.
