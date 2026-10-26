# Multiplayer menu entry restored

25 October 2026. The isolated Windows research build now displays **Multiplayer** below Continue in the ordinary U3 profile menu. Selecting it opens the retained **Play Online / LAN Party** choice page. Campaign buttons remain present. This restores the menu entry and first callback transition, not playable multiplayer.

Launch with `Run-U3-Multiplayer-Menu.cmd`. Press Cross at the title screen normally. The script builds a private prepared image, copies saves/settings into its private test directory, and runs until closed. Installed game/runtime files remain unmodified. The prebuilt helper is `work/u3_menu_entry.bin`; rebuild it with:

```
"D:/1. PC Games/Uncharted-windows/python/python.exe" outputs/build_u3_pool_blob.py --source outputs/u3_menu_entry.c --out work/u3_menu_entry.bin
```

The native PS4 `menu.bin` retains `BUTTON_MULTIPLAYER` in PAGE_DEMOPROFILE. The helper copies its complete relocated 0x68-byte definition into spacer slot 2 of PAGE_PROFILE, preserving collection counts and the other entries. It checks the expected page counts, IDs, source event/title/condition and destination spacer before writing. Repeated calls accept only an identical prior copy.

The working hook replaces the five-byte lookup call at guest 0x6496ec, immediately before frontend menu construction at 0x649660. It calls the original lookup, preserves its result, modifies the data, and returns the original header. The initial MainMenu factory hook at 0x574870 was too late: the controls had already been constructed. Its run showed no new button. Runtime watch logging also excludes offsets >=0x2000000, explaining why injected-helper breakpoints produced no printed line; absence of those lines does not establish that helper code failed to execute.

The unmodified native callbacks registered for Multiplayer (0x66e500) and LAN (0x66e510) are single RET instructions. Mode `menu-callback` now replaces the Multiplayer RET with a guarded relative jump to a new RX wrapper that tail-calls native ShowPage 0x651940(&frontend, PAGE_MULTIPLAYER_CHOICE=0x1e66a744, 0, 0). This follows the native frontend call at 0x621080. It changes no network-success flags and performs no factory substitution. Backend orchestration and networking remain unfinished. Play Online currently returns to the same choice page; LAN still has its empty callback. No successful lobby or match is claimed.

The launcher now uses `menu-callback`; `menu-entry` preserves the earlier visibility-only experiment. Runtime run `menu-callback-1792937932482370800` visibly reached the choice page, and native ShowPage was observed with its expected page hash. Repeat `menu-callback-1792938030157212200` selected LAN Party with Down and returned to the normal profile menu with Circle. Both stayed alive until their test limits. Captures/logs are in `outputs/evidence/multiplayer-choice-*`.

Fresh PS3 evidence identifies the original button registration at 0x27ca24, OPD 0x1178d70 -> callback 0x290bd8, which invokes NetInit at 0x2901e0 and tears down on failure. LAN uses OPD 0x1178868 -> 0x27fdf0. These establish that real initialization must be reconstructed behind the choices; merely showing a page does not provide sessions. Evidence: `ps3-frontend-callback-registration.c`, `ps3-multiplayer-entry-wrappers.c`, and `ps3-entry-callback-descriptor-refs.json`.

`outputs/tests/test_u3_menu_probe.py INPUT BLOB` verifies callback/menu-hook branch targets, relocated helper readback, original-source preservation, and rejection of changed signatures. It passed against the installed prepared input and current helper.

A second isolated run (`menu-entry-1792937587023495700`) repeated the visible button, selected it with Down at 21 seconds, and pressed Cross at 25 seconds. The native Multiplayer callback breakpoint at 0x66e500 hit, then the menu remained visible. This verifies selection and real event dispatch. Screenshot and logs are retained in `outputs/evidence/multiplayer-button-selected.png` and `multiplayer-menu-click.log`.

Validation: the helper host test checks full pointer-preserving copy, untouched neighboring entries, idempotence, and rejection without mutation. Run `work/test_u3_menu_entry.exe` after compiling `outputs/tests/test_u3_menu_entry.c`. The working runtime run is `work/runtime-tests/menu-entry-1792937541524804700`: construction lookup and return breakpoints both hit; captured normal menu contains Multiplayer; runtime stayed alive until the 35-second test limit.
