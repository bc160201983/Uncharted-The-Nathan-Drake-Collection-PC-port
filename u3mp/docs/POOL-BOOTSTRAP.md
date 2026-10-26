# Pool bootstrap continuation

## Result

The missing native PS4 NPC/pickup pool initialization has been reconstructed and exercised in the Windows research runtime. It gets beyond the previous null pickup-table read at `0x73e6ef`. No Multiplayer button or playable multiplayer has been restored.

## Evidence behind the implementation

PS3 `NetGameManager::Init` (`0x305d2c`) allocates 16 NPC tracker nodes and 256 pickup nodes. Fresh PS3 pool helpers recover the bucket and sentinel algorithm. Surviving PS4 initializers at `0x1003850` and `0x125dc70` independently establish the native header layout, 16-byte pool alignment, allocator callback ABI, ownership flag and scoped allocator usage.

Native PS4 pool fields are: bucket array +0, free head +8, node stride +0x10, capacity +0x14, pool +0x18, extra sentinel +0x20, allocation base +0x28, bucket end +0x30, bucket count +0x3c, initialized byte +0x40, lock +0x50.

The native bucket prefix is rounded up to 16 bytes. With PS4 pointer/node sizes, allocations are **3544 bytes** for 16 NPC nodes (stride 200) and **18512 bytes** for 256 pickup nodes (stride 64). Both include the extra self-linked sentinel node and sentinel bucket slot. These correct the earlier PS3-only size estimate, which did not include native alignment padding.

`u3_pool_init.c` reconstructs this setup. It pushes native allocation scope 5 through `0xda1590`, uses the allocator's virtual Allocate method at +0x48, and performs the same paired stack pop as native code. The first diagnostic version used the factory's temporary allocator and hit the engine's SpawnProcess ownership assertion. The scoped version resolves that assertion; no assertion bypass is used.

The helper uses the game's System V x86-64 ABI. `build_u3_pool_blob.py` compiles only this new source and links its checked internal PC-relative relocations. `build_u3_bootstrap_probe.py` places it in a new RX segment and calls it from the guarded resource-bootstrap wrapper. Original binaries remain unchanged, verified by SHA-256 after the tests.

## Validation

- Meaningful host tests cover native field offsets, pool alignment, full forward/backward free-list traversal, sentinel links, bucket emptiness, guard bytes, insertion/removal, exhaustion, malformed capacities, insufficient memory and repeat-initialization rejection.
- Two scoped game runs advanced beyond the previous reset failure.
- The instrumented repeat records execution at `0x73e6ef` with table end pointer **RAX=0x1065cc5800**, replacing the previous zero state.
- The test later faults at `0x678e84`; RDI is zero at a virtual call through the interface pointer stored at `g_net` +8 (`0x17d6428`).
- This happens on a second online-manager construction: the diagnostic still permanently substitutes the single-player MainMenu factory and forces the Leave state. The first pass finishes cleanup, then the same substitution enters online code again. This is an experimental route, not a recovered normal multiplayer startup sequence.

The next work is restoring proper frontend/NetInit orchestration and network-service creation. Supplying no-op interface methods or skipping the null call would not establish a working network state.

## Files

- `u3_pool_init.c`: new native initializer.
- `build_u3_pool_blob.py`: compiler and checked internal linker.
- `tests/test_u3_pool_init.c`: reproducible invariants tests.
- `Run-U3-Pool-Bootstrap-Probe.cmd`: builds and runs the current bounded experiment; no input needed. It is expected to reach the later service-interface failure.
- `evidence/pool-helper-tests.txt`, `pool-helper-build.json`: validation/build identity.
- `evidence/pool-bootstrap-confirmed.log`, `.json`, `pool-bootstrap-manifest.json`: runtime evidence and exact image changes.
- `evidence/ps4-native-pool-initializers.c`, `ps4-allocator-scope.c`, `ps4-fixed-map-insert.c`: fresh native corroboration.
- `evidence/post-pool-test-input-identities.json`: unchanged source identities.

## Remaining limitations

Allocation context 5 is corroborated by the surviving native container initializer, but a complete network-owned allocation/teardown lifecycle has not been restored. Transport, session orchestration, authentication, tickets and signaling remain unimplemented in this work. The patch is for the Windows prepared-image runtime, not an installable console SELF.
