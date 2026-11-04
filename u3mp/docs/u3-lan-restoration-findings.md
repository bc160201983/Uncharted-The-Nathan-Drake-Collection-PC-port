# U3 LAN restoration — findings log (PS4 CUSA02320 U3 1.02 in BilalReloaded's PC port)

Builds live in `outputs/builds/lan-lobby-vX.Y` (u3mp/builds/ in this repository layout); launcher `Test-Uncharted3-LAN-Lobby.cmd`.

## What Bluepoint disabled and how each piece is restored
| Area | PS4 finding | Restoration |
|---|---|---|
| Menu datasources | All 27 dispatchers (0x763f40…0x7704d5) only call non-whitelisted sources (anything but 0x63/0xcf/0xe1) when net-game+9 (0x17d5c61) is set | v4.4 patched the gate to net-game+8; **removed in v4.20** — the original check passes because the MP-active byte is now raised at LAN start, as on PS3 |
| Main MP list / party list | b3 / ad sources are RET stubs | Retagged to cf hook, native C (v3.x) |
| Custom Game options | Game Type 0x26, Map 0x27, rules 0x28–0x37 intact | Work once MP-active is set (7 modes, 11 maps) |
| Start Game | STARTMATCH 0x66edf0 stub; countdown page 0x73a73dd8 missing | Hook calls countdown-done 0x66e9b0 directly — v4.5 |
| Session backend | 0x17dd768 / g_matchSession+8 null | Guest LanBackend (PS3 vtable 0x10e7f68 slots ×2); Create fills member 0, ready flag +0xa1, result +0xa4 |
| MP-active flag | 0x17d5c61 never set (PS3 sets it in NetInit 0x290ca0) | Set at LAN start — v4.6 |
| Find a Party room list | 0x17dd758 null (source 0xd0) | Points at zeroed room table — v4.6 |
| Network heap | kAllocNetwork (0x14) / NetworkProcess (0x15) never registered (allocator table 0x18417b0; size record 0x16f9978 = 0) | Aliased to wrapped-malloc allocator (9) — v4.7 |
| Net clock | 0x17c3580 never assigned; must start near 0 at match start | Own clock at state+0x2000, zeroed on Start Game, ticked via redirected engine Clock::Update call at 0x10ff75c — v4.11 |
| Start-load flag | EstablishConnection 0x67fad0 waits for session+0x1338 (normally set by settings event 0x71c780) | Backend slot 14 (PS3 +0x38, room broadcast) raises it on the host — v4.12 |
| Abort flag | 0x16e7170 starts as 1 in data; cleared only by state enter 0x666c20 / Leave exit 0x6775f0 | Cleared (with net error 0x17d5c80) on Start Game — v4.13 |
| Player creation | PS3 state 0x11 (0x2b4444) creates a net player per session member (f_304948 = PS4 0x72eca0); PS4 kept only the debug bot-fill branch (flag 0x17d7b07) | Restored in backend slot 11 (PS3 +0x2c lock, called at state 0x11 start) — v4.14 |
| Emblems | 0x66b1d0 runs before 0x66b510 fills its table 0x1b67e60 (dirty byte 0x17c3440) | Helper kind 10 wraps call 0x72c17c: fills table first, then setup — v4.16 |
| Cinema film recording | WaitReady 0x67f240 opens a film via stream 0x17c3ba8; PS4 film path missing ("/-4851") → null bitstream | 0x67f240 patched to RET (no film recording) — v4.17 |
| MP character list handle | DC symbol handle 0x17c3b40 statically initialised with id 0 (0x689440); used by 0x687d30 (29 calls) for 0x38-byte entries keyed by character id | Id set to 0x916e03b0 (net.bin MP character list, keys like 0x6afcc877) — v4.18 |
| Session done-flags | Playing ended at once: done flags (+0xb0…+0xf0) were 0 because Session Init/Reset 0x7a2360 (magic 0x1234 at +0x12e0) never ran | Start Game calls 0x7a2360 on g_matchSession before Create — v4.19 |
| HUD-net "David" class | PS4 vtable 0x16dafe0 ↔ PS3 vtable 0x10ff0e8 (same 9 slots, same "David" name). Slot 3 (PS4 0xc3b770 ↔ PS3 0x6603f4) draws a script-set HUD image for the local player (color table 0x12c75f0[0x6e], texture f_97202c, draw f_971d60). PS4 keeps only the checks and traps (int 0x41 at 0xc3b822) whenever a local net player exists | Trap replaced with NOP — v4.21 |
| MP entity-spawn filter | 0x72fbc0 (12 callers, MP-only) binary-searches entity tag tables (entity+0x68 {data,count}) with no count check; PS4 level data has null data for empty tables → fault 0x72fd62 while London loads | Empty tables pointed at a never-matching sentinel before the call (count stays 0) — v4.22 |
| Late-join team pick | Playing update 0x67e6e4 → 0x746790 → 0x7462b0 (session not full) → 0x737eb0 "leading team" returns -1 on a tie (0-0 at start); caller indexes team table 0x17dd778+idx*0x130+0x5c16 without checking → fault 0x746563 | Call 0x74654e wrapped: -1 → team 0 — v4.20 |

## Shortcuts still in place (not LAN-restored until this list is empty)
| Shortcut | Stands in for (PS3) | Proper fix |
|---|---|---|
| Start Game calls countdown-done 0x66e9b0 directly | STARTMATCH 0x66edf0 + countdown page 0x73a73dd8: party size, team balance, all-ready checks, 10 s countdown | Port the PS3 STARTMATCH handler and countdown page |
| Guest LanBackend with stub slots; slots 4/11/14 hand-written for a solo host | PS3 LAN backend class 0x371920 (vtable 0x10e7f68) and room protocol: UDP 9012 discovery, TCP messages c9/ca/cb/cc/cd/ce/cf/d1 | Port the class slot by slot; add a bridge for two PCs |
| Find a Party room table zeroed | Room list filled by discovery replies | Comes with the backend port |
| kAllocNetwork/0x15 aliased to malloc | Dedicated network heap registered at boot | Register a proper heap of the PS3 size |
| Net clock owned by helper, ticked from a redirected Clock::Update call | Engine net clock created by NetInit 0x290ca0 | Find the PS4 clock slot that NetInit used and create it the PS3 way |
| Emblem guard (kind 10) | Emblem table filled during MP boot | Find the PS3 init order and call 0x66b510 at the same point |
| 0x67f240 RET (no film) | Cinema recording to a valid film path | Restore the film path / stream |
| HUD-net slot 3 trap NOP'd (0xc3b822) | PS3 0x6603f4 draw: only when tracker+0x8a5 and the script object's +0xb4 image is set | Port the draw using the PS4 2D draw functions |
| Empty tag-table sentinel for 0x72fbc0 (12 sites) | PS3 counterpart of 0x72fbc0 not yet compared — confirm how PS3 handles an empty tag table | Compare with PS3; remove if the PS3 function checks the count |
| City of Brass killvis skipped (v4.23): Start Game clears map-list entry +0x40 for san-upper-multi-killvis | PS3 loads it (0x72b570 path, entry +0x38/+0x40); PS4 ships an empty stub | Port the PS3 killvis level data |
| Tie guard at 0x74654e | Unknown — PS3 counterpart of 0x7462b0 not yet checked; possibly the team table 0x17e338c is filled by a step still missing | Find the PS3 function and the team-table init; remove the guard if the table is the real cause |

## Match start flow (PS4, all present)
0x66e9b0 → backend Create → wait popup 0x471007a1 with state machine 0x6a5fa0 (state 3 → 0xb → 0x28 → 0x11 → 0x2c) → load page PAGE_CUSTOM_LOAD 0x69e10dad, handler 0x6a7310 (0x2c → 0x2d → 0x2e → 0x2f → 0x30) → script event starts net game process.
Net game process (vtable 0x167e2c0; state records 0x1b688d8…0x1b68a28, stride 0x20): [initial 0x66da80] → EstablishConnection (0x66db40, update 0x67fad0) → LoadLevels (0x66dc10, update 0x67f5f0) → WaitReady (0x66dcd0, update 0x67f240) → Playing (0x66dd90, enter 0x67e980, update 0x67d760) → … → Leave (0x66e230, enter 0x678b00). GoTo slots: +0x1b8 LoadLevels, +0x1c0 WaitReady, +0x1c8 Playing, +0x1e8 round results, +0x1f8 Leave.
Net state 0x1d = in game (reached in v4.17).

## Session layout (g_matchSession 0x17d6420)
+8 backend; +0x18 member count; +0xa1/+0xa4 create done/result; done flags +0xb0…+0xf0; members at +0x680 stride 0x120 (+0x25 active, +0x2c id, +0x34 connection, name at +0); +0x12e4 self id; +0x12e8 host id; +0x1338 start-load flag.
Net players: NetGameManager 0x17cb060 +0x390 + i*0x9c0 (+0x21c team, +0x365 ready, +0x5e4 leaving); host player at mgr+0x380.
Team table: entries 0x130 bytes at 0x17e338c (+0 type 1/2, +2 score, +0x66 flag); team count 0x7210a0(mgr).

## DC data notes
net.bin symbol table at 0x28: 0x195 entries of (u32 id, u32 type hash, u64 value offset). 0x11fd4c0(id,1) finds the loaded module holding a symbol (respecting the module's script condition); sentinel 0x1b3af40.

## Status
v4.19: Playing no longer ends at once; crash in the Playing update on the late-join team pick (index -1). v4.20 adds the tie guard and drops the datasource gate patch.
v4.21 (London): level load starts; crash in the MP entity-spawn filter on an empty tag table. v4.22 adds the sentinel.
v4.20: past the tie guard (menus still fine without the gate patch); crash on the HUD-net slot 3 trap. v4.21 NOPs it.

## Map loading (v4.21 finding)
- Match runs (net state 0x1d for ~28 s, then 0x26 → 0x13) but no map loads: menu backdrop stays.
- Map list = DC symbol 0xdec97a4 in net.bin (PS4 entry 0x98 bytes, PS3 0x54). Fields (PS4 offset): +0x10 preview image "net-levels/<map>" (PS3 vtex1/net-levels/*.dds — missing on PS4, explains no map pictures), +0x28 main pack, +0x38 killvis, +0x58 sub-level list, +0x68 "own world" flag, +0x70 world level, +0x80 stage.
- Netgames (symbol 0x3b9a067d, 0xd0 bytes, 89 on PS4 vs 195 on PS3) are gametype × map combos; +4 = map id.
- NetGameManager level setup 0x72b570 calls LevelWantLoad (0x10eb990) for the map's pack/killvis/sub-levels; LoadLevels 0x67f5f0 only loads the +0x70 world when +0x68 is set (same on PS3).
- PS4 disc has: ldn-multi-comp-pak + ldn-train-stage (London, killvis and world-london-mp missing); san-upper-multi + stage + sky (Sanctuary, comp-pak missing); yemen-airport-multi-pak (Airstrip, sub-levels missing); world-multiplayer. Yemen, Chateau, Syria, Desert, Ruins, Highrise, Facility, Museum packs are absent. PS3 has all of them in build/main/pak23.psarc (8.7 GB).

- v4.22 London run: no crash, in game (state 0x1d) until closed; ldn-multi-comp-pak loaded; screen grey with some gates. London sub-levels (multi-all, ldn-train-multi, ldn-train-multi-lit, tunnels, tc01–06, killvis, world-london-mp) are not on PS4 — only stage + comp-pak.
- Sanctuary (map entry 4, 7th in the TDM list): sub-levels san-upper-multi (64 MB), san-upper-wide-multi, sky-san-upper-multi, stage, killvis present; missing san-multi-comp-pak, san-multi-pak, multi-all.

- City of Brass stall: level-mgr registry = pak23_ps4.txt (LoadLevelDefs 0x10e9bd0); unlisted levels (san-multi-comp-pak, san-multi-pak) get state 0xc and do not block. Blocker is san-upper-multi-killvis: listed, but it is the only 244-byte stub pack on the disc with no -ingame pack (contents removed); it sits in level state 2 (Level::Update 0x10f3500 waits for every package at level+0x80 (count +0x78) to reach state 0xc, then required symbols and sound banks), so the load queue never advances to san-upper-multi / sky / wide.
- PS3 GOTY map list has 65 entries incl. DLC (gompa, ice-cave, fort, shambhala, warzone, temple, village, train, graveyard, oldquarter, oasis, streets); PS4 has the 27 base entries only.

- **v4.23: City of Brass loads and plays** (state 0x1d): san-upper-multi (64 MB), san-upper-wide-multi, sky-san-upper-multi, stage, world-multiplayer, san-multi-bgs bank all loaded. Camera ends up outside the map; "Lone Wolf" splash shown. Not loaded (absent on PS4): multi-all (PS3: multi-all-ingame — MP weapons, pistol/rifle-mp-base, magazines, common-multi bank), san-multi-comp-pak (respawn/capture-flag regions, elevator, chests), san-multi-pak (net death region), some mp-kit/mp-hero packs.

## Spawn chain (v4.23–v4.26)
- Round start (0x73ad00 "Play Task") picks a spawner with 0x732bd0 and calls NetPlayer spawn 0x787a00(player, spawnerId, 0) (= PS3 f_360af8). For a local player without a character it stores the spawner at +0x3b4, sets request +0x420 and returns 0; the per-frame net player update 0x789a60 (called from 0x722e00/0x7230f0) then creates the character process (0x119b250 at 0x78aed2 → handle at +0x10/+0x18, clears +0x420).
- Spawner list = entity spawners of the map's main pack level (0x72fb00 → map entry +0x30 → level-mgr GetLevel). City of Brass: main pack san-multi-comp-pak is missing on PS4 → empty list → spawner 0. London: ldn-multi-comp-pak present → spawner 0x1123e984, but still no character after 30 s (v4.26 counts the process spawns in 0x789a60).
- PS4↔PS3 net player offsets: +0x21c↔+0x1e4 team, +0x365↔+0x315 ready, +0x420↔+0x3c0 request, +0x5e4↔+0x51c leaving, +0x5ed↔+0x525 level loaded, +0x5f9↔+0x531, +0x5fa↔+0x532 (only set by PS3 late-join event 0x8b→0x8a handler f_2c7ac4). PS3 f_2e983c = PS4 0x6b4b20 (RequestRespawn), f_35ebf8 = 0x7863b0, f_2f4fd8 = 0x6b7b30 SendNetEventPlayerInfo. Script natives net-respawn-all-players(-team) = 0x79cf90/0x79d070.
- PS3 player loop (0x2b4444) also sends message 0x106 to each member; PS4 has neither sender nor receiver. PS3 receiver (lobby 0x28a124) just calls f_2f4fd8(0/1), which PS4 state 0x11 already does.

- v4.26 London: spawner 0x1123e984 picked, request pending, but neither process spawn in 0x789a60 (0x78a32b, 0x78aed2) ran — an earlier condition in the net player update blocks character creation. Spawn work parked here.

## Menu datasource inventory (PS4 stubs vs PS3), v4.26
PS4 dispatcher entries that are RET stubs or missing, with the PS3 implementation to port (PS3 address, size):
- 0xb3 LIST_MAIN_MENU (PAGE_MULTIPLAYER) → 0x342d84 (1336; friends/presence list mode 0x885ef7e9) — currently replaced by a hand-written LAN list
- 0xad party list → 0x332910 (jump table)
- 0x1e BUTTON_MULTIPLAYER_STARTMATCH → 0x347d34 (528) — label handled natively
- 0x39 LIST_CINEMA → 0x32f1a8 (316)
- 0x31 map preview → 0x347a20 (788) (needs PS3 net-levels textures)
- 0xa6 / 0xa7 loadout weapon lists → 0x32be2c (120) / 0x338ab8 (136)
- 0x43 LIST_BOOSTERS → 0x33b574 (492); 0x44 LIST_MEDAL_STREAKS → 0x331e6c (220)
- 0x6e body customisation → 0x32cf4c (268); 0xbf LIST_SPOILER_CLASS → 0x332f64 (208)
- 0x7–0x10, 0xae, 0xb2 → 0x34634c (2380, shared); 0xb0 missing → 0x3432bc (344); 0xe2 missing → 0x32bea4 (96)
- 0x11 → 0x33e85c, 0x13 → 0x33e0ac, 0x50 → 0x344f7c, 0xc2 → 0x3330d4, 0xd3/0xd4/0xd9 → 0x32c114/0x347618/0x32bdd8
- 0xda/0xdb/0xdc YouTube/Facebook lists (online services — skip); 0x23/0x2d are 4-byte stubs on PS3 too.
Missing pages vs PS3: PAGE_CONTROLS_BUTTONS, PAGE_CONTROLS_CAMERA, PAGE_FMV_UNLOCKS, PAGE_GALLERY, PAGE_NET_TROPHIES (PS4 replaced with its own pages). Countdown page 0x73a73dd8 missing.

## Menu restoration (no porting) — started v4.27
- Datasource ids shift: PS4 id >= 0x95 is PS3 id - 1 (e.g. LIST_MAIN_MENU PS4 0xb3 = PS3 0xb4 f_334ee8; LIST_PLAYERS 0xad = PS3 0xae f_34634c). Earlier inventory corrected accordingly.
- PS3 f_334ee8 lobby list: 12-byte rows {text, icon, action}. LAN tables: 0x11bf908 (6 rows: Custom Game 0x66d63005/0x7e2c2757/0x7d593924, Find a Party 0xebd77105/0x23e9be55/0x52f3d9f6, Profile 0xa2aa79ea/0xb675afdb/0xc4acc827, Cinema 0xcb379354/0x12d18838/0xb85cb12a, Options 0xfb08c743/0x2c68ff83/0xe7d7dbbb, Splitscreen 0xd7d4ccad/0x88c52b24/0xcd127c29) or 0x11bf8d8 (4 rows) when PS3 net-game+0x3fd is set. Online tables 0x11bf950/0x11bf98c add Find Game 0xbd1d2c53, Community 0xe62d7aeb, Store 0x310c1d9f. Op 6 returns last index; op 2 swaps Splitscreen for "Leave Splitscreen" 0x2b93528e when f_383ff8 is true; op 4 icon when slot id 0xf5a294f3 (PS4 event[0x14]); PS4 icon fill = 0x7521a0 op 4 layout (type 3, id/flag at [7], index from *(0x1b24bc8)+0x28 table).
- PS3 f_265c94 (Menu::OnButton) = PS4 0x64ce90(menu, action, 0); PS4 kept Profile 0xc4acc827 and Options 0xe7d7dbbb, dropped Custom Game / Find a Party / Cinema / Splitscreen (those live in PS3 lobby handler f_28dbf8 = PS4 0x672450 RET stub).

## Assets
PS4 install has world-multiplayer, san-upper-multi (likely Yemen), MP skies, 178 mp- character packs. Missing: mp-hero-skel-bones-glow.pak, mp-chloe-skel.pak, a few mp-kit packs and emblem textures; level packs for desert/facility/highrise/chateau/airstrip/museum multi were not found. Map preview source 0x31 is a stub.

## Lobby backdrop, menu scene and camera (v4.60–v4.63, October 26)
- Map names: map 4 / `net-levels/san` / `san-upper-multi` is **City of Brass** (earlier notes say Sanctuary — wrong). Sanctuary is not in the PS4 data.
- PS4 still ships two MP lobby stages: `san-upper-multi-stage` (+ `sky-san-upper-multi`) and `ldn-train-stage`, plus `menu-default` (408 KB, PS3's fallback stage). Each stage's `-ingame12` pack has one spawner, `menu-spawner-1` (sid 0xfbef8374).
- Map list entry (0xdec97a4, stride 0x98): +0x80 stage name, +0x88 stage sid. Stage loader 0x743420(netgame mgr 0x17cb060, bool, mode): with bool 0 it preloads the stage of the selected netgame (mgr+0x661c index → netgame +4 map id → map entry) through LevelWantLoad 0x10eb990; mode 3 hides/unloads the current stage. The current stage name lives at mgr+0x74b8 (char[64], asserted < 0x40). Called from NetGameManager level setup 0x72b570 (0x72c0f1, 0x72d30f).
- In v4.60 the native loader **did** load City of Brass after the map was picked in Custom Game (and briefly the full `san-upper-multi` + wide + sky, then stage + `menu`). The camera stayed at the title camera, so the view was a close-up of stage walls over a grey floor.
- Menu scene manager = global 0x17bba90 (MenuScene; +0 state, +4 substate, +0x1460 lobby character handle, +0x14a0/+0x14b0 spawner locator, menu_state at +0x15f8). Per-frame update 0x5b7240 (called from 0x63c231): in state 2 it waits until package `anim-proto-multiplayer` (sid 0x67adb399) and up to 50 loadout packages (0x17f9938+4, gated by +0xf70) are loaded, then 0x5b6640 places the lobby character at `menu-spawner-1` (spawner lookup 0x10ec0c0, locator 0x10e6f30).
- **Orphaned on PS4 (no callers left):** 0x5b7020 = enter MP menu scene (state = 2, then 0x997230 on 0x17f9938 = MP loadout package requests from the profile's character parts); 0x5b4e60 = lobby camera setup: hashes the stage name at mgr+0x74b8, finds the stage level (state 8), reads `menu-spawner-1` into +0x14a0, positions party slots relative to it and spawns/updates the camera process (0x101c320 type 0x41d1d900, then 0x101b530). PS3 equivalent of 0x5b4e60 is 0x20784c, called from the frontend loop at 0x28a2e8; PS3 0x206e60 = PS4 0x5b6640.
- Title camera for comparison: 0x5752a0 spawns camera `menu-camera-3` (sid 0xc6c90233) from the title-screen level; 0x575230 kills it.
- Package/level manager 0x1afcf10: +0x28/+0x30 want-load list, +0x38/+0x40 second list (entries 0x18 bytes {u64 name hash 0x10e95b0, u32 sid, u16, u8 flags @0xe, u64}); find level 0x10eb1d0, find package 0x10eb380, level state at +0x30 (8 = loaded), show/hide 0x10f25b0.
- Helper: v4.61 requests and shows the stage + sky at LAN start; v4.62 calls 0x5b7020 (scene state 2/1 confirmed by memory watch, no loadout packages requested, `anim-proto-multiplayer` never loaded); v4.63 requests `anim-proto-multiplayer`, writes the stage name to mgr+0x74b8 when the helper loaded the stage, and calls 0x5b4e60 once the stage level reaches state 8 (again when the stage changes).

## PS3 → PS4 asset conversion (v4.5x)
- PS3 foreground/background mesh blocks decode exactly (positions, UVs, normals, tangents, triangle order, skinning) — format in PAK-AND-MESH-FORMATS.md, decoder `paktools/ps3geom.py`.
- `paktools/ps4actor.py` writes a PS4 actor pak from a PS3 actor plus a PS4 template: converted PS3 Drake (`hero-london`) renders and animates in game, using the PS4 remastered textures for materials whose names match exactly.
- PS4 rules found on the way: VRAM_DESC +0x34 stream flag must be 2 (streamed) or 3 (resident), 0 is invalid; texture hash = sid(short path with .ndb); GEOMETRY texture entries and material bindings use the full path without `.ndb`, VRAM_DESC/TEXTURE_TABLE keep it; pak pages must stay below 2 MB (loading heap page size assert); ARTGROUP bucket count comes from the PS3 artgroup.
- Inventory: 679 actor packs missing on PS4 (190 disc, 294 patch 13, 135 patch 18 incl. 45 taunts, 60 DLC). PS3 1.19 map list has 81 entries incl. DLC maps; PS4 has 27.
- PS3 ANIM clips ('CD' v0x0107) vs PS4 ('AC' 0x0104): same sections with per-section endian swaps and inserted mask words; converter not finished. PS4 has 15 disc taunts, 1.19 has 44.
- DC relocation bitmap: PS3 is MSB-first within a byte, PS4 LSB-first.
