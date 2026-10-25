# Hero initialization dependencies

## Required skeleton: exact package/code match

Read-only script `work/rizin-mp/audit_hero_skeleton.py` validates the original
package chunk table, finds tagged records with header magic12345, and compares
the exact fields used by the runtime. Output with complete paths and hashes is
`work/rizin-mp/hero-skeleton-dependency.json`.

- Patched `actor27/ps4/hero-default.pak`, SHA256
  `118e8606ddabac43933e7ac03590cc598afe072597af79daac617d1fb1b55260`:
  FOREGROUND_SECTION_2 header at file242db0, +28 = `27e57f43`.
- Base `actor27/ps4/proto.pak`, SHA256
  `19c515e9a13d6e8219128573c6bd3a2cce8f36d2a04ae187a710f323447e0715`:
  JOINT_HIERARCHY header at file784b40, +50 = `27e57f43`.

Player vtable+170 is96f300, returning the skin string at object+2720. The base
actor initializer720b30 gets that skin from process descriptor+18 -> +18.
Vtable+178 is993b20: it hashes the skin, tries looks.bin registry9dbf0022,
then falls back to the loaded foreground table (`14705f0`, count`14775f0`,
stride38), matching package SID at entry+20 and reading model pointer+28.
The loaded skeleton table is1585080/count1587880/stride28. 991550 compares
the required skeleton SID against skeleton+50, returning0cf60009 if the
nonzero SID has no matching loaded skeleton.

The `hero-default` SID6328516f has no row in the inspected looks.bin table;
the foreground fallback therefore supplies the required27e57f43. This is the
same ID stored by proto's joint hierarchy, not an inference from filenames.

`GameInitInternal` calls969320 at3f8410 before it requests proto at3f879c
(string RVAeb05cd). The original proto request uses56c200(manager1130390,
"proto",0), then ORs Level flags+18 with60. A loader barrier56bf60 occurs
later at3f88df. Thus preloading only the hero mesh before the early auto-spawn
does not supply its skeleton. The parent/map agent may test a verified native
proto preload before hero; this report makes no claim that it fixes every
Player initialization dependency.

Evidence: `hero-skeleton-contract-1.02.asm` and `hero-proto-order-1.02.asm`
under work/rizin-mp. The missing optional effect1/hero-default.bin log entry
does not establish a missing skeleton. Runtime validation subsequently confirmed it: stage-callback-counts returned
0cf60009 with the failure text Skeleton Not Loaded, on a worker thread
outside the spawning thread scope. The stage-proto-hero run then returned
Player::Init=0 with no failure callbacks and a valid generation3 hero after
90 seconds. This confirms skeleton initialization, not gameplay control.


## Following dependencies and animation control

`work/rizin-mp/audit_hero_animations.py` reads tagged records in the original
package chunks. Its JSON output `hero-animation-inventory.json` records paths,
hashes, counts and examples. The base proto package has **1548 ANIM records**,
including walk, walk^idle and walk-stairs-up, as well as its joint hierarchy.
The other packages contain animations but no JOINT_HIERARCHY:
anim-proto-multiplayer235, anim-proto-carry-objects137, anim-proto-shield60.
These three are already declared dependencies of stage-multi in the retained
net.bin catalog (`map-evidence/map-dependencies.json`). A direct actor/level
load does not by itself prove the complete catalog list was streamed. However,
a T-pose alone does not establish that one of these extra packages is missing.

The animation binding path is part of Player initialization:

- vtable+d8 ->9938f0 allocates a200-byte AnimControl, stores it at Player+190,
  calls3918f0(AnimControl, Player+188 skeleton-instance, 10 or30), and stores
  the AnimControl back into SkeletonInstance+10.
- vtable+f8 ->7247a0 calls3923e0(AnimControl, e94543da). The latter ID is the
  exact CRC of anim-player, not anim-net-player. A positive result propagates
  as initialization failure; result0 means that this stage passed.
- AnimControl+8 is m_pAnimData (named by the native assertion in7258f3);
  +130 is the controller count, +148 points at instance info.
- Animation initialization is not permission to call these constructors twice.
  They allocate TLS-owned engine objects and expect the original process heap.

Other mandatory script lookups after base initialization are already retained:
722c3e looks up2e928895 in characters-collision-settings.bin (symbol table58),
96cc29 looks up4885284a in water-splashes.bin (table78), and96cbbb/cbca use
12f06fea/fb83c5df from player-aim-settings.bin. The spawn configuration
c66bbbc7 is in player-settings.bin at1d8, pointing to the value recordce0.
LookupPointer a15e70 asserts on missing symbols; these are data modules,
not additional actor packages. Raw SID matches are recorded in
hero-script-sid-matches.json; they are not a claim that every match is an
export. Animation and player modules appear in the native startup module
list, e.g. relocation1044828->ea17c8 anim-player; later modules remain in that same startup list.

## State scheduling and pause checks

The original factory calls Init(+20), post-init(+28), then Go(+30), provided
the process state-machine pointer+50 exists. Player Go is71df30 and ends at
vtable+358 ->8b7010: that function schedules the native Move state descriptor
RVA14f8a08 in stateMachine+8 and sets process flags bit100. StateMachine+0 is
the current state instance; +8 is the requested next descriptor. Reading
these is a useful diagnostic, but writing fabricated state objects is unsafe.

ProcessManager RVA115f1a0, routine9b8d50, builds a bounded update list at+d38
with count+2d30;9bd660 only considers processes whose24-bit flags at+34
satisfy `(flags & 5) == 1`. The state dispatcher9bdb90 exits before state
transition/update when its process time context dt<=0 AND
`(u16(process+34) & 0x2400) == 0x2000`. The post-update dispatcher9b9260
uses the same gate at9b93e3 before calling vtable+58. The context pointer is
process+70, its current frame/tick is+0c and delta time float+14.

The scene capture and unchanged stick snapshots do not yet identify which
condition blocks gameplay. Next read-only comparison: process flags, time
context frame/dt, state current/next, AnimControl data/count/instance pointer.
The established constructor chain and packaged baseline animations make
these checks more informative than loading arbitrary packages.

Evidence: actor/hero/process Capstone extracts in work/rizin-mp,
hero-next-dependencies-1.02.asm and hero-animation-controller-1.02.asm.

## Activation verified; runtime is already in Death

Further Rizin evidence closes the suspected missing-Go path. The auto-spawn
routine 969320 calls 975190, whose call at 975528 enters factory 9b5c20.
The factory invokes Init at 9b64c1, post-init at 9b64d8, and, when both
return nonpositive and process+50 exists, Go at 9b6787. It then requires
stateMachine+8 to be nonnull or reports `Missing go`. Address 9be450 is
the failure reporter, not the creation routine. The descriptor-based
spawn routine 975670 also reaches 975190 at 975a8d or 975d23; it does not
provide a separate missing Go step. Its caller a610a5 is in the retained
game-task region; a complete campaign task contract is still unproven.

Go 71df30 invokes Player vtable+310, +528, then +358 (8b7010). The last
function requests the original Move descriptor at 14f8a08 through the
vtable+c0 state acceptance predicate. State instances contain owner at+8
and descriptor at+10. A descriptor contains class-name pointer at+0,
state-name pointer at+8, constructor at+10, SID at+18 and priority at+1c.

Parent runtime evidence `stage-proto-state.json` had a valid hero, flags
6ab1, time-context dt about 0.01697 and a current state with no pending
transition. ReadProcessMemory subsequently resolved that state's descriptor
to RVA14e17d8, class Player, name Death, SID4f875693, constructor79c5a0.
Thus the persistent body is a dead player, not evidence that Go never ran.
This makes spawn location and creation timing the immediate investigation.

The native guard 96f380 allows leaving Death only for SIDa6a64bf5.
The Death::Exit assertion at 7a1d36 explicitly names this SID **DebugFly**:
`Self().IsProcessDead() || Self().IsNextState(DebugFly)`.
It is not a respawn state. Calling Go or merely moving the dead actor does
not satisfy the original transition contract. A normal restart must follow
the proper disposal/recreation path; no guard bypass is implemented here.

New raw evidence in work/rizin-mp: hero-autospawn-factory-tail-1.02.asm,
hero-go-move-factory-1.02.asm, hero-descriptor-factory-1.02.asm,
hero-descriptor-factory-tail-1.02.asm, hero-move-state-layout-1.02.asm,
hero-death-state-1.02.asm and hero-death-exit-SID-1.02.asm.

## Spawn and camera argument contract

Rizin traces of 975190 establish the following SysV inputs. RDI is the
32-byte world transform used as InitDesc+10 (position vec4, then xyzw
quaternion). RSI is a separate 32-byte camera world transform. RDX is the
skin name, RCX the player configuration (fallback SID459d0e4e), R8 optional
parent/spawner context written to InitDesc+50. R9 is not consumed here.
The boolean loaded at9751aa is the seventh argument, on the stack, not R9.
It becomes PlayerDesc+20 and then global1116180+fd8 during Player::Init.
AutoSpawn supplies configc66bbbc7, R8=0 and stack boolean0.

The RSI camera pose is copied into a temporary PlayerDesc field and sent
unchanged to the current camera handle1114208 using message0a605802 at9754c5.
The retained handler434e02/435125 copies it to camera+370/+380 when its
control list has exactly one entry. For active control type90870002,
435b7e additionally writes control+1a0/+1b0 and frame+50/+60, then resets
frame velocities. The handler does not add a third-person camera offset.
Replacing both input transforms with the same player-spawn pose therefore
initially colocates them; it does not establish a useful camera framing.

The original descriptor-driven path975670 resolves separate player and
camera spawners, calls4bdfe0 for each at975ced/975cff, then passes these
distinct poses to975190 at975d23. R8 comes from player-spawner+60.
Base Init991626 reads that context's SID+28 and validates its parent handle
at+30/+38, then obtains parent.vtable+190 and calls992c00 to attach the actor.
Passing zero, as autoSpawn does, is appropriate when only an independent
static world pose is available; an arbitrary spawner pointer is not that
context layout.

For reference, autoSpawn's native pose calculation starts from camera C,Q.
Its player xyz is C+(0,-1,0); camera xyz is C-forward(Q), where forward is
the quaternion's rotated positive-Z direction. Both preserve Q. Constants
at e4da10..e4da40 and the exact shuffle sequence are saved, so this relation
can be derived without guessed distances. Prefer the existing camera
spawner pose where the level supplies one.

Evidence: hero-create-rizin.asm under work/local-lobby/map-evidence and
work/rizin-mp/hero-camera-message-contract-1.02.asm,
hero-camera-handler-tail-1.02.asm, hero-spawn-parent-constants-1.02.asm.

## Deferred-spawn ragdoll assertion

The stage-wrapper-trace run reached native player/camera spawners and then
failed at512f44, MapToRagdoll line297, on its first mapping (RBX=0).
512d10 receives world-space joint cache in RDI, skeleton/ragdoll data in
RSI, output Havok transforms in RDX, mapping object in RCX and a flag in R8.
The source at5159f1 is AnimControl+8 (the SkeletonInstance/animation data),
then+38. Source joint records are32 bytes. Mapping+10 points at40-byte
records with int16 boneA at+0, boneB at+2, offset transform at+10/+20/+30;
mapping+18 is count. Output records are48 bytes.

The source quaternion is normalized at512de4..512e14 without a zero-length
guard. isOk1068e0 rejects NaN in position/scale xyz and calls106fc0, which
rejects NaN quaternion components and a squared norm too far from1.
994080 also normalizes the initial world quaternion without a zero guard.
Thus an all-zero pose reaching the factory can produce this exact chain;
the assertion alone does not establish a mismatched Havok asset.

The parent identified a concrete risk in the experimental bridge: thread-local
pose arrays were filled across native calls that can migrate between worker
threads. A readiness flag observed on another TLS instance could accompany
zeroed arrays despite a previous successful pose log. The replacement uses
immutable shared poses with atomic publication and a single-use claim.
The subsequent stage-static-spawn test passed: Player::Init returned0,
init_entries=1 and failure_entries=0. No assertion or bone transform was
bypassed or overwritten by this investigation.

If needed after that test, the fault frame permits exact read-only diagnosis:
RBP-f8 is source cache, RBP-f0 output cache, RBP-120 mapping object. Compare
the first referenced input and output joint before changing any asset.
Raw evidence: hero-ragdoll-assert-1.02.asm, hero-ragdoll-map-inputs-1.02.asm,
hero-ragdoll-validation-1.02.asm, hero-animdata-create-1.02.asm,
hero-skeleton-init-cache-1.02.asm and hero-joint-cache-constructor-1.02.asm.

## Validated native player and camera, isolated startup

The stage-static-spawn boot log confirms successful native initialization.
Three read-only snapshots under work/local-lobby/map-evidence establish
actual control rather than only a rendered model:

- stage-static-spawn-state.json: valid generation3 hero at
  (22.698759,1.000003,20.606701), state IdleAmbient; camera at
  (23.865629,2.098778,24.296110).
- stage-static-forward.json: state Move and hero position
  (20.680817,1.000000,13.385100), with following camera at
  (21.831823,2.098778,17.079494).
- stage-static-after-camera-jump.json: camera moved to
  (17.352621,2.098778,15.359037), quaternion changed from approximately
  (0.007565,0.987368,0.049717,-0.150248) to
  (-0.024889,0.867843,0.043698,0.494287). This proves camera rotation;
  this snapshot alone does not prove that a jump occurred.

Visual evidence in a local capture folder (not in the repository):
UC2-stage-native-spawn.png, UC2-stage-native-forward.png,
UC2-stage-native-camera.png and UC2-stage-native-jump.png. The forward image
was inspected: Drake stands upright at normal third-person framing on the
retained stage's white grid geometry, rather than the earlier distant body.
This validates local native movement and camera in the isolated command-line
startup test. It does not yet validate launching from the lobby, returning
to that lobby, combat, or gameplay replication between peers.
