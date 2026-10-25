# Original frontend-to-world transition: static investigation

Target is the reconstructed Uncharted 2 PS4 1.02 ELF already identified in
MULTIPLAYER-PC-BRIDGE.md. No guest call or runtime patch was added by this
investigation. This is a partial contract, not a safe working map launcher.

## Retained menu path

`work/rizin-mp/ui-world-transition-1.02.asm` follows the real menu dispatcher:

- BUTTON_NEWGAME ID `b861a261` branches at `5c9894`. It searches the real task
  registry `6e51b48f/67723261`, checks installed data and advances UI state.
  This is not a generic map-name entry point.
- BUTTON_NEWGAME_NORMAL ID `d8e3038b` branches at `5c9f02`. Special SKU paths
  find an existing task node via `a572d0(task SID, node SID)`, then branch to
  `5caeb3`. The full-game path writes pending difficulty and enters state10;
  it should not be reused casually because it belongs to new-game/save logic.
- The shared transition `5caeb3..5caf4c` first checks MenuHost+1014. Unless
  already in states0,5,6, it constructs the existing small UI event through
  `ccf830`, dispatches stage3 through `5cd5c0`, sets state5/time0 and a 1.0
  float at +101c, then dispatches stage0. It finally calls
  `a57dc0(realNode, 1, &delay, 1)`. Delay is0 or300 based on a game global.
  The explicit UI event layout and lifetime remain owned by original code.

`a572d0` resolves a task and its node from the original registry. `a57dc0`
uses node+38/+88 and other real descriptors, cleans up earlier task state,
and dispatches the replacement node. Passing a native lobby model, map CRC or
fabricated descriptor here is invalid. `5cd5c0` is a large state machine, not
a simple hide-menu method. Evidence: `work/rizin-mp/ui-task-transition-1.02.asm`.

## Original return path

The menu's confirmed quit action branch at `5cb1eb` uses `a5b190()` to obtain
the appropriate original menu task node, then executes the same UI transition
and task activation sequence. `a5b190` chooses based on SKU/network state;
it is not an unconditional fixed pointer.

A complete existing helper at `a5b430` is explicitly named
`GameTaskPlayStartMenu` by its assertion string. It calls `a5a270(1)`, clears
the loader's requested-level/actor counts at `1130390+5a98/+5aa8`, obtains
the real node through `a5b190`, asserts if absent, then calls
`a57dc0(node,1,&zero,1)`. This is a stronger candidate for task teardown/return
than manually unloading a single Level. It has not been called experimentally;
caller/thread/context requirements and save/menu effects still need runtime
validation. Evidence: `work/rizin-mp/ui-playtask-console-1.02.asm`.

## Why play-testing is not yet a sandbox launcher

The original tasks.bin task `play-testing`, ID `3090c1b8`, lives at file10f0.
Its start edge leads to node `playtest` ID `7c478a36`, followed by
`destructible-test-1` and `treasure-test-1`, then the end sentinel. The task
has no world/script name at +10/+20, but each node carries additional unresolved
script/config references. This does not establish that it accepts stage-multi
or creates a suitable camera/player. Exact source path, nodes and edges are in
`work/rizin-mp/play-testing-graph.json`.

The retained smoke-test command formats `play-task <task> <node>` and invokes
the debug menu path `Tasks|Play Task|<task>|<node>` through `4ac890` and the
debug root menu, asserting if that root does not exist. It is not a standalone
console callback for starting arbitrary maps in the shipping frontend.

## Task teardown contract, 24 October follow-up

The isolated CLI stage run now has a valid native hero (`Init=0`),
`IdleAmbient`/`Move` states, movement and camera rotation. See
`MULTIPLAYER-HERO-DEPENDENCIES.md` and the `stage-static-*` snapshots. This
does not yet validate launching from the lobby.

Further Rizin disassembly establishes that `a5a270(1)` is substantially more
complete than a node-list reset:

- It removes active node list entries and destroys each node's three valid
  process handles through `9be760`, then resets runtime flags.
- At `a5a666` it calls `9691d0()`. This validates the actual global hero
  handle at `115bd20` (slot pointer, nonzero generation, object+44 generation)
  and calls `9be760(hero)` at `96920d`. It also preserves the current camera
  pose (+370/+380) through original message `0a605802`. Thus a hero created
  outside a task is explicitly included in teardown.
- At `a5a66b` it calls `3f4ca0()`, which traverses the gameplay process group
  (`ProcessManager115f1a0+c40`), marking appropriate live processes for native
  destruction. It resets loaded level state through `573230`, camera fields
  +338/+33c and player configuration +fd8, among other engine subsystems.
  This is not equivalent to unloading package memory.
- At `a5a81b..a5a884` it runs the original MenuHost transition to state5
  (excluding already-transitioned states0,5,6), including constructor
  `ccf830` and stage3/stage0 dispatch through `5cd5c0`. A separate fabricated
  UI event or direct frontend visibility write is unnecessary for this path.

Thread requirement is explicit: `3f4ca0 -> 573230` reaches
`Background::SetRot` and `SetPos`, with `ndjob::IsGameFrameJob()` assertions
at `5732d6` and `573393`. A host polling thread or arbitrary guest worker is
not an acceptable call context. TLS job identity must be validated at a
candidate in-engine command/frame boundary before an experiment.

Evidence files under `work/rizin-mp/`:
`frontend-task-teardown-1.02.asm`, `frontend-task-processes-1.02.asm`,
`frontend-hero-cleanup-1.02.asm`, `frontend-world-cleanup-1.02.asm`,
`frontend-stop-callers-1.02.asm`, `frontend-stop-wrapper-1.02.asm`.

The retained debug task callback at `a53280`, event type4, has a branch
`a532dc..a532e6` which calls `a5a270(1)` and returns without activating a
replacement node. Therefore an idle world after stopping tasks is a retained
engine behavior, not an invented task descriptor. Other branches restart a
real selected node. This callback still requires its original debug control
object, so it is not itself a generic bridge ABI.

## Candidate integration boundary and remaining proof

The existing native Start control registration at `5edadf`, control SID
`70e80076`, is the appropriate UI intent boundary; the current bridge keeps
it disabled. A future implementation can queue an explicit native launch
request there, consume it at a verified GameFrameJob boundary, stop the old
tasks with `a5a270(1)`, and then perform the already-proven level/wrapper/proto
load and spawn sequence. This is a proposed sequence, not a runtime-verified
contract. It must wait until the old hero handle is invalid before spawning
exactly one new hero. Spawn poses must retain the immutable atomic publication
used by the successful CLI experiment.

The original return helper `a5b430()` remains the strongest candidate: it
invokes teardown, clears requested level and actor counts, resolves the real
start menu node and activates it. Experiment-owned level pin bits and host
launch state need lifecycle handling; none should be cleared by guessing at
unrelated ownership. The helper has not yet been executed from the local map.

`a57dc0` is not an alternative arbitrary-level loader: after stopping tasks
it invokes the selected real node's compiled script at node+68 through the
bytecode interpreter `a15430`, passing the real node as one argument. That
script determines the new task behavior. Its final `a595d0` call traverses
task graph configuration; it does not supply a level-name override.

## Remaining boundary

### Concrete deferred execution site

`3fbcf6` is a five-byte `call a5aa00`, bytes `e8 05 ed 65 00`, inside
`GameFramePostRenderUpdate` (`3fbbc0`). The latter's exact name/function pairing
is registered at `3f5acc..3f5ae8` using string `e79d71` and function pointer
`147ee10`. A call-site adapter should preserve the original `a5aa00()` call,
then consider the queued local launch request, and return to `3fbcfb`.
The caller immediately calls `a16360`; no result from `a5aa00` is consumed.
`a5aa00` consumes no incoming argument (its entry initializes its registers
from globals), making `void ABI adapter(void)` appropriate at this one site.
Its other callers at `9fedcd` and `a52e50` should remain untouched.

Why this point is stronger than a menu callback:

- MenuHost update/render at `5d0530` has already returned. It runs inside
  `GameFramePreRenderUpdate` (`3fc0b0`), registered by name at `3f5a78`.
  The root `GameLoopUpdate` (`3facb0`) calls PreRender at `3fb596`,
  `4b90d0` at `3fb59b`, then PostRender at `3fb5a2`.
- The original task update at `a5aa00` already calls `9691d0()` at `a5ae17`
  and `a5ae2c` when activating replacement task nodes, and creates new native
  processes at `a5aec8`. Hero lifecycle operations belong to this phase.
- Consuming only after that task update returns avoids changing its node
  lists while its iterator is live. Later in the same PostRender function,
  `3fbfe3` calls `9bed80`, then `3fbff6` calls `9bec90` on the process manager
  list. Hero invalidation still needs to be observed, not assumed immediate.

The GameFrame predicate is inlined, not a proven standalone callable address.
The exact implementation at `573276..5732c8` is:

```
manager = *(uintptr_t *)(image + 0x118e280);
tls_offset = *(int64_t *)(image + 0x104ad40);
raw = *(uint64_t *)(guest_tcb + tls_offset);
index = (raw & 0x8000) ? (raw ^ 0x8000) : UINT64_MAX;
is_game_frame = manager && index <= 5 &&
               (*(uint8_t *)(manager + (index << 8) + 0x1b8) & 0x40);
```

Read exactly eight bytes for `raw`. A read-only runtime accessor must require
an already-attached guest thread and bound the signed TLS offset inside its
actual TLS allocation. Do not use Windows FS directly: the loader rewrites
guest FS accesses into the runtime's guest-TCB TLS slot. Validate mapped reads
and capture the predicate at the actual call site before enabling transitions.
The static call chain supports this candidate, but a name alone does not prove
the active worker flag in a given run.

One outstanding lifecycle risk is explicit: `a5aa00` can activate eligible
nodes whose runtime state is0 (`a5ad11` onward). After `a5a270(1)` resets nodes,
observe whether native task conditions remain inactive on subsequent frames.
Do not assume stop means the original task scheduler is permanently suspended.

Evidence: `frontend-frame-job-1.02.asm`, `frontend-frame-phases-1.02.asm`,
`frontend-task-frame-point-1.02.asm`, `frontend-frame-predicates-1.02.asm`,
`frontend-posttask-contract-1.02.asm`, and
`frontend-frame-hook-fingerprints-1.02.asm` in `work/rizin-mp/`.

Known: original node lookup, menu transition sequence, node activation and a
named start-menu return helper. Unknown: an existing node suitable for a local
multiplayer map, its real initialization factories, spawn selection, camera and
input ownership, and runtime teardown. The startup `level stage-multi player`
diagnostic has now validated local assets/hero/camera. In-menu launch, repeated
launch, game-frame synchronization, cleanup completion, and return to the
existing LAN lobby remain unverified. No transition runtime patch was made.
