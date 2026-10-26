# Uncharted 3 multiplayer restoration — continuation

Date: 25 October 2026

**Status: Multiplayer is now visible and opens the retained Play Online / LAN Party menu in the isolated Windows research build. Selection and Back navigation were tested. Session initialization and networking remain unfinished; playable multiplayer has not been restored.**

Launch the menu experiment with `Run-U3-Multiplayer-Menu.cmd`. See [MULTIPLAYER-MENU.md](MULTIPLAYER-MENU.md) for the verified menu hook, evidence and limitations.

Latest continuation: [POOL-BOOTSTRAP.md](POOL-BOOTSTRAP.md) documents the reconstructed allocator, tests, repeated runtime result and remaining startup/lifecycle work.

## What changed in this investigation

The previous menu experiment crashed at guest offset `0x99610b` after looking up `0xbd3bb3b9`. The game's own CRC table identifies that hash as `*custom-characters*`. Native PS4 `u3data/build/main/dc1/ps4/net.bin` still defines it and contains 57 valid character records. Its 405 export names reconcile with `net.dci`. This resource is present; the failing code has not registered it.

Fresh PS3 Ghidra analysis shows `NetInit` explicitly loading the `net` module and its dependencies before reading multiplayer settings. Fresh PS4 analysis recovered the matching blocking loader at `0x011f85e0`, dependency enqueue at `0x011fcd70`, and native allocation tag `0x12`.

`build_u3_bootstrap_probe.py` now builds an experiment for the installed PC runtime's prepared U3 image:

1. Use the earlier early-entry online-manager substitution for the reproducible resource experiment. The natural-transition alternative was separately tested and did not enter the online manager.
2. Hook the zero-argument `TaskManagerOnline` factory at `0x66d8c0`.
3. Check whether `*custom-characters*` is registered.
4. If absent, load native PS4 `net` through the game's blocking dependency loader.
5. Check registration again; stop at an explicit `UD2` diagnostic marker if it failed.
6. Resume the factory's exact displaced instructions and original body.

The early-entry mode also retains the older diagnostic Leave-state selection and fallback to frame time for missing session-clock access. It therefore does not reproduce a complete normal NetInit sequence. The hook lives in a new RX image segment, rather than a guessed free area in existing code. It uses relative calls, preserves the native stack alignment, and avoids repeated module acquisitions once the required symbol exists. Module lifetime/teardown is not yet integrated.

## Validation completed

- Parsed native PS4 DC headers and exports; decoded all 57 custom characters.
- Compared PS3 and PS4 character schemas: PS3 records are `0x3c` bytes, PS4 records are `0x70`; raw PS3 data cannot be transplanted.
- Decompiled the PS4 blocking loader and independently checked its assembly and native callers.
- Ghidra decoded every injected instruction; all branches and calls resolve to their intended addresses.
- Checked the augmented runtime container's length, segment counts, segment bounds, and injected bytes.
- Rejected an already patched image, truncated image, wrong format, and altered loader signature.
- Confirmed the installed prepared source image's SHA-256 was unchanged after patch construction.
- Initial baseline and bootstrap attempts exited **21 before guest execution**, with Windows error **487** while reserving the PS4 address range. A suspended-process map showed private host allocations inside the required range.
- A process-local launch layout fixed that obstacle. `u3_child_process.py` applies ASLR layout policy `0x220000` to the test child only, validates the reserve range before resuming it, and changes no installed executable or global setting. The baseline then reached the normal U3 menu.
- Two early-entry resource-bootstrap runs loaded `net.bin` (327751 bytes), reached the old fault instruction `0x99610b` with **RAX `0x105faac7a8`**, and progressed to a new null read at `0x73e6ef`. The earlier character-data lookup failure is therefore reproducibly resolved.
- Fresh Ghidra analysis identifies the next failure as an uninitialized NetGameManager pickup registry: bucket table at manager +`0x6590`. The reset operation dereferences a null bucket base. The native manager constructor initializes these fields to zero; the missing storage initialization remains to be restored.
- The natural-menu experiment reproduced the offline Friend Stats dialog using only scripted Cross inputs at 12, 20 and 28 seconds, then loaded the existing campaign chapter. No online-manager breakpoint was hit. The earlier screenshot with mixed human/scripted input is not evidence of multiplayer activation. The dialog belongs to the ordinary campaign route.

The current installed launcher reports version **0.6.0**. REA is connected but rejects the console SELF and PowerPC ELF targets on its current Windows route. Analysis continued with the existing Ghidra **12.2** projects, copied into this workspace and opened read-only.

## Local test entry points

- `Run-U3-Bootstrap-Probe.cmd`: starts the reproduced early-entry resource-bootstrap experiment. It currently stops at the known pickup-registry crash.
- `Run-U3-Baseline.cmd`: starts an isolated baseline using the same installed runtime.
- `Run-U3-Natural-Route-Diagnostic.cmd`: preserves the unsuccessful natural-transition experiment for comparison; it currently follows the campaign route.

All use `D:\1. PC Games\Uncharted-windows\python\python.exe`. They read the installed prepared image and original PS4 assets. Existing saves/settings are copied into the private test directory; originals are not used for writes. Images, private saves, captures and logs go into this workspace's `work/runtime-tests/`. Close the game normally when finished. These launchers run until closed; neither overwrites the game or installed launcher.

The resource-bootstrap test enters automatically and needs no button presses. The baseline and natural-route diagnostic do not add a Multiplayer button. Automatic research runs used the runtime pad file, and their button timings are recorded in each result JSON.

## What remains unresolved

- Restore the missing NetGameManager container/storage initialization and reach a visible multiplayer frontend. The character resource has been registered, but a multiplayer UI has not been demonstrated.
- Recover any additional frontend dependencies and the PS4 equivalents of the PS3 NetInit orchestration.
- Implement actual socket transport, session orchestration, identity/tickets/signaling and U3's custom services as required by a local private match. The earlier runtime source still contains disconnected networking stubs; synthetic probe identity is not real networking.
- Load one native multiplayer map and spawn a local player, then demonstrate two clients exchanging gameplay state.

No playable LAN, online matchmaking, or native PS4-console compatibility is claimed. The prepared-image patch is for the Windows research runtime, not an installable console SELF patch.

## Reusable evidence

- `hash-resource-report.md`, `hash-resource-findings.json`, `inspect-u3-dc-actual.json`
- `inspect_u3_dc.py`: read-only native resource inspector
- `ps3-bootstrap.md`, `ps3-resource-schema.json`, fresh PS3 decompilations
- `ps4-blocking-loader.md`, `.c`, `ps4-blocking-loader-assembly.txt`
- `evidence/bootstrap-stub-disassembly.txt`, `bootstrap-validation.json`, `bootstrap-patch-manifest.json`
- `evidence/resource-bootstrap-reproduced.log` and `.json`: reproduced resource-load success and next failure
- `evidence/baseline-running-result.json`, `baseline-main-menu.png`, `controlled-friend-stats-dialog.png`, `controlled-campaign.png`
- `runtime-address-layout.md`, `evidence/runtime-child-mitigation-layout.json`
- Initial pre-workaround failures: `evidence/baseline-result.json`, `baseline-boot.log`, `bootstrap-result.json`, `bootstrap-boot.log`, `runtime-address-conflicts.json`

The screenshot supplied in the request is used as context for the comparison strategy, not as evidence that U3 multiplayer has already been restored.
