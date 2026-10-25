# Uncharted 2 multiplayer — reverse-engineering notes

Goal: bring Uncharted 2's competitive multiplayer back on this native PC port.
Addresses are image vaddrs of `big2-ps4_Shipping.elf` **1.00** (base game, no update) as
prepared by `tools/probe_uncharted.py` (`UNCHARTED_UPDATE=0`), unless marked 1.02.

## Version choice
- 1.02 (the port's default) dropped most of the session/connection layer: the strings
  `net-connection.cpp`, `net-session-manager.cpp`, `ReadPacketHeader`, `NET_SM_*`,
  `netSessionManagerRoomEventCallback` exist in 1.00 only.
- 1.00 boots in the port (60 s at 60 FPS, main menu). The port's 8 patches for 1.02 are skipped
  ("no variant matches this game version").

## Imports (both versions)
- No `libSceNpMatching2` import at all (no PSN rooms), no `sceNetSocket/Sendto/Recvfrom`,
  no `sceVoice*`. `libSceNet`: only `sceNetInetPton`.
- POSIX sockets from libkernel: socket/bind/connect/listen/accept/send/recv/select/setsockopt.
  Every `socket()` call is `socket(AF_INET, SOCK_STREAM, 0)` (TCP): call sites 0x6a896c,
  0x7ee2c6, 0x7ee2fb, 0x7ee50d, 0x7ee5b1 — look like Naughty Dog dev-tool connections
  (listen/accept/select servers), not game traffic. Not confirmed.
- NpSignaling (5 in 1.00): ActivateConnection 0x301183, GetConnectionInfo 0x301337/0x3013d7,
  GetPeerNetInfo 0x30153a — inside the net session code.
- Runtime trace: `GOW3_NET_TRACE=1` prints every sceNet/sceNp/sceVoice call. At boot U2 calls only
  `sceNpGetNpId` (signed out) and `sceNpTusSetDataAsync` (fails).

## Code locations (1.00)
- NetConnection reliable-packet ack loop: around 0x2c8850 (assert "ReadPacketHeader" at 0x2c889d,
  `notify->packetSequenceNumber == m_highestAckedSequence + i + 1`).
- NET_SM state names: 0x2c89f8..0x2c9336 (state machine / name lookup).
- net-session-manager.cpp: 0x2f6cbd (`g_sessionEventQueueNum`, room event callback);
  net-session.cpp SetData: 0x2f75f5.
- NetPlayerData (profile stats, trophies): global 0xf44840, accessor 0x2ee940, methods 0x2ee5e0..0x2efb20.
- Debug options block: 0xe65230 (net simulation: +0 packet loss %, +4 latency ms, +8 duplication %).
- Theater/replay (NET_SM_REPLAY, StartNetGame 0x2d6870): 0x8fd462..0x8ff7f9.
- Game init: `GameInitInternal` (big.cpp) 0x14a4f0; calls `NetCryptInit`.

## Naughty Dog debug menu (DMENU)
- Built at init: 0x1c0650 ("Dev Menu" root), stored in 0xebbdc8 and the menu manager 0xebc770
  (+0x30 root menu). Flags near 0xebc760/0xebc761/0xebc764.
- "Net..." submenu builder: 0x2e3570. Items include: Disable Min Player Limit, Disable Time Limit,
  Fill Scoreboard with Fake Players, End Game: Time Up, Force Host Team, Force Even Team Counts,
  Force Matchmaking Min Players, Force Show Matchmaking Vote, Force Rank, Packet loss/Latency/
  Duplication, Print Net Events, Test Snapshot, Player Arrows, Golden Guns / Double Money.
- Not yet known: how the menu is opened (button combo / flag) and whether retail draws it.

## Reachability in 1.00 (static: direct calls, vtable relocations, lea, raw pointers)
Dead (present in the binary, never referenced):
- NetConnection packet I/O: 0x2c8560, 0x2c86a0, 0x2c86f0 -> 0x2c8740 (ack loop, ReadPacketHeader).
- net-session-manager.cpp 0x2f6c80 (room event callback), net-session.cpp SetData 0x2f75e0,
  net-predictor.cpp 0x2f3d00, flow.cpp 0x2baf60.
Alive:
- net-game-manager.cpp 0x2d5b00 / StartNetGame 0x2d6870: only from theater/replay vtables (0xe31xxx).
- net-player-tracker 0x2f04e0, net-snapshot 0x2f8b70: used by game/animation code.
- Signaling code 0x301130 (sceNpSignalingActivateConnection) reached every frame from the main
  loop: 0x14b7e0 -> 0x2ba180 -> 0x2b8d60 -> 0x301130.
Meaning: Bluepoint shipped the multiplayer transport, sessions and flow as unreferenced code.
They can be called again (hooks), but their wiring (object creation, vtables, update calls) must
be rebuilt.

## Debug menu (closed lead)
- Opening: MenuManager::HandleInput 0x1cd800 opens [mgr+0x30] (Dev Menu) on pad bit 0x2 when byte
  0xe7149c != 0. Forcing [0xebc788]=root, [0xebc77c]=1 sets the menu active, but nothing is drawn:
  the draw function 0x1cdcf0 is unreferenced in retail. Options can be set by writing their
  variables instead.

## Multiplayer button and page texts (done)
- tools/uc2mp_menu.py writes a menu.bin overlay (UNCHARTED_MOD_DIR, Lancer-UC2-Multi.cmd): the hidden
  BUTTON_MULTIMEDALS of PAGE_NEWPROFILE becomes "Multijoueur" -> PAGE_MULTIPLAYER. Works in 1.02.
- String ids: CRC-32 MSB-first, poly 0x04C11DB7, init 0, no xor. Text in core.psarc text1/ps4/<lang>.common.
- The four "UNTRANSLATED STRING!!!" / "Coopération" rows are NOT the page's buttons: they are its
  profile stats panel (PS3: Competitive Wins / Competitive Kills / Money / Co-op, each with a value).
  The buttons (Matchmaking, Custom Game, Cinema, Leaderboards, Trophies) are menu.bin records with
  label source 0x20 (text asked from code, commands 51/52/53/131/136).

- Text files: count, (id, offset) pairs sorted by id, offsets relative to the string pool after the
  pairs, UTF-8. French = text1/ps4/3.common: bd1d2c53 "Recherche de partie", 66d63005 "Partie
  personnalisée", cb379354 "Cinéma", c072e0a6 "Coopération", f75b3a65 "Multijoueur", bdb14168 "_".
- Mode table rows (1.02 0xdef390; PS3 0xb40c50, identical): 0 "_", 1 matchmaking, 2 custom game,
  3 cinema, 4 47a936dc (no text), 5 f1b9728c (no text). Rows 1-3 match commands 51-53 (arg - 50).
- Label lookup: PS4 0x655d39 `TextLookup(0x1066660, table[idx].label)` (0xcddc00, fallback
  [db+0x10] = "UNTRANSLATED STRING!!!"); PS3 0x4b78a0 same code (idx [r3+0x34], db -0x7fec(r30),
  lookup 0x9296cc). PS3 reaches the table through a TOC pool (toc -0x7868 -> 0xdba68c, -0x7f28).
  That table is not what the page draws (probes never hit it).
- The stats panel is drawn by 0x59ddb0 (1.02), which looks four labels up with `mov esi, id`
  immediates: f67d9ad2, 70c81d2e, c0cd1b00 (text removed from the Collection) and c072e0a6
  "Coopération". gow3_pc/src/uc2mp_text_bridge.h hooks the text lookup 0xcddc00 and answers the
  removed ids from uc2mp_texts.h (Competitive Wins / Competitive Kills / Money, 7 languages, chosen
  from the active "Multiplayer" text). The earlier MP_MENU_LABEL_PATCHES (stats rows renamed to
  Compétition / Partie personnalisée / Cinéma) were a misreading and are gone.
- Buttons: uc2mp_menu_bridge.h answers commands 51/52/53 with the game's own texts (bd1d2c53,
  66d63005, cb379354) and 131/136 with our ids UC2MP_TEXT_LEADERBOARDS/TROPHIES; only 52 is enabled.
  Do not write labels into those menu.bin records: label source 0x2 draws the button centered
  (seen on screen 2026-10-24), so tools/uc2mp_menu.py leaves them untouched.
- Confirmed on screen 2026-10-24: five buttons and the four stats labels in French. Still missing
  vs PS3: icons, stat values, player name/rank badge, daily players, Kills line, bottom bar.
- Live probes: Lancer-UC2-Multi-Sondes.cmd, GOW3_EXEC_WATCH_FILE=work\sondes.txt (re-read while the
  game runs; up to 4 image offsets; "Watch:" lines in boot.log with rax..r9 and callers). The game
  process restarts on Collection game switches (LoadExec): live memory pokes do not survive that.

## PS3 reference (Uncharted 2 PS3 1.00, BCUS98123)
- Decrypted EBOOT: <ps3recomp>\UC2MP\game\BCUS98123\EBOOT.ELF, with function table
  out\EBOOT.functions.json and imports out\EBOOT.imports.json (Tony's earlier PS3 recompilation project).
- The PS3 build imports the whole network stack: sceNp (67), sceNp2 = NP Matching 2 (31), sys_net
  sockets (23), cellNetCtl, cellVoice (17), cellHttp/cellSsl. Use it as the map of the wiring that the
  PS4 Collection lost (same source, same strings and ids; PowerPC, 32-bit pointers).

## Original entry investigation (2026-10-24, Rizin)

The existing direct PAGE_MULTIPLAYER link bypasses the original button callback.
PS3 menu.bin uses BUTTON_MULTIPLAYER (id ed9d8ae1) with no direct page action.
The PS3 callback is 43b320 -> bootstrap 438d98 -> asynchronous NetInit 447c28.
Bootstrap failure calls cleanup 4388a0, not another initialization path.
See MULTIPLAYER-PS3-ENTRY.md for the resolved TOC/OPD chain and object initialization contract.

The same button ID is still registered in both PC-compatible PS4 executables:

| Build | Registration | Callback | First instruction |
|---|---|---|---|
| PS4 1.00 | 2bd4b1..2bd4c2 | 2bb1a0 | ret (c3) |
| PS4 1.02 | 5ed9db..5ed9f5 | 5eb160 | ret (c3) |

This confirms the original entry callback is empty in both builds. It does not prove
all downstream multiplayer functions are absent. Returning SIGNED_IN from runtime stubs
would not recreate the missing initialization and is not a restoration.

Reproduce with tools/uc2mp_entry_audit.py ELF --rizin RIZIN_EXE --out NEW_DIRECTORY.
The tool rejects unknown SHA-256 identities, verifies callback bytes through ELF PT_LOAD,
and preserves Rizin output/warnings. Three successful audits are in work/rizin-mp/audit-*.
REA's Windows native provider rejected the ELF targets; its Rizin execution tool also
failed to launch. Rizin 0.9.1 CLI successfully inspected both architectures directly.
No runtime behavior was tested by this investigation, and no game patches were deployed.

## Next
1. Map the PS3 NetInit session objects and constructors to surviving PS4 1.00 functions,
   checking layouts, virtual methods and update callers before invoking them.
2. Implement a local session lifecycle in place of the historical PSN/ticket sequence;
   establish a real local member and readiness/failure states before showing the lobby.
3. Restore the BUTTON_MULTIPLAYER callback route only with that lifecycle implemented.
   Keep the current direct-page overlay as an independent UI diagnostic.
4. Supply and test packet send/receive, then verify two local processes exchange game state.
   A selectable menu alone is not multiplayer completion.

## Lobby match rules (2026-10-24)
- PAGE_LOBBY's rule rows are live: commands 60 score limit (25/50/75/100, default 50),
  63 rounds (1/2/3/5, default 1), 61 time limit (5/10/15/20 min or none, default 10),
  62 friendly fire (no/yes; a k13 row: the menu opens its own Non/Oui list). Lists in
  runtime_uc2mp_catalog.h; Uc2mpSettings keeps the indices (UC2MP_SETTINGS_DEFAULT).
- Row labels are the game's MENU_MULTIPLAYER_CUSTOM* texts in its language; TIMELIMIT
  (07774a0d) and FRIENDLYFIRE (9b7aa1a9), which the Collection lost, come from uc2mp_texts.h.
- LAN protocol version 3: snapshots carry the 4 rule indices after count (30 settings bytes),
  receivers reject out-of-range indices; MAX_PACKET 505 (a full 10-player snapshot).
- Playlist (55), variation (59) and co-op difficulty (65) stay hidden (empty text).
- Checked offscreen in the real menu: every row shows and changes. The rules are stored and
  synced; no match logic reads them yet (scores and timers need the gameplay sync).

## In-match pause (2026-10-24)
- A game started on a map from the command line never allows pausing: the pause gate in the
  menu manager update (1.02 0x5cf4fd...) needs menu manager (image+0x1136640) +0x105c set, which
  only the Collection layer's gameplay hook (0xa61320, bputil/ucc-gameplay OnHook) does. The
  spawn bridge sets it once the hero exists (matches only).
- The pause page choice (0x5c7bb0) takes PAGE_PAUSE_MULTIPLAYER only in network mode
  ([0x14ce97c] >= 2); in a match the spawn bridge removes the jl at 0x5c7c05 (0f 8c 06 02 00 00,
  checked) so the multiplayer page shows: Options / Joueurs muets / Quitter partie.
- Quitter partie -> PAGE_MULTIPLAYER_LEAVEGAME_CONFIRM, command 128 (original 65e3b0: question
  2df029e8 "Voulez-vous vraiment quitter ?", choices 6318c53f Quitter / 81493ae7 Annuler). The
  dispatcher only calls commands in network mode ([0x14ce97c] > 1, e.g. 65ff30), so the menu
  bridge answers 128 itself. Request 9 (set index) also follows cursor moves; request 0x12
  (slot 100b4e8, 65ff30) comes only on confirmation: Quitter restarts the game on its main
  menu (LoadExec "eboot.bin ispartofuccollection"); Annuler returns to the pause page.
- The direct map test (Tester-Carte) runs without the local lobby, so without the menu bridge:
  the pause page shows there, its leave dialog stays empty. The lobby's match launch has both.
- Checked offscreen through the real path (lobby, launch, pause, dialog): cursor moves never
  leave, Annuler returns, Quitter reaches the title screen.

## Match loading screen (2026-10-25)
- PAGE_MULTIPLAYER_LOADING is only two empty lists in the Collection: the PS3 drew its
  loading visuals in native code. The port's overlay draws its own instead
  (gow3_overlay.cpp MatchLoadingScreen): mode, map card, map name, rules, "Chargement de la carte...".
- Map cards come from the player's net.pak (art/general/net/maps/<name>-color BC1 +
  -trans BC4, 1024x512, PS4-tiled, upside down): tools/uc2mp_loading_art.py writes
  <overlay>/uc2mp/loading/<map key>.png; the local launcher runs it once per overlay.
- The lobby writes GOW3_LOADEXEC_FILE + ".uc2mp" (uc2mp_match_info.h: map, mode, map name,
  rules) before LoadExec; the match process reads it when the spawn bridge accepts a match map
  and shows the screen; the spawn bridge's on-spawned callback hides it once Nate exists. A
  direct map test has no such file: it shows "Multijoueur" and the catalogue map name.
- The offscreen frame dumps do not include the overlay; checked by Tony in a window (2026-10-25).
  Closing logs "UC2MP: match loading screen closed after N frames" on stderr.

## PS3 lobby reference (RPCS3 screenshots from Tony, 2026-10-25, BCES00757 1.09)
- MULTIPLAYER page: icon per button (Matchmaking, Custom Game, Cinema, Leaderboards,
  Trophies (0%)), profile panel under them (name, Competitive Wins/Kills, Money, Co-op, rank
  badge and booster/weapon icons), right column Party Leader / Party Members / "N daily players",
  bottom row "Money: <name>: $0". Footer: Profile / Party Up / R1 Store / SELECT / BACK.
- CUSTOM GAME page: mode icon at the left, rows Start Game, Game Type (mode pictogram + name),
  Map (map card + name), Variation (Standard), Score Limit "50 kills", Time Limit "20 minutes",
  Friendly Fire "Off"; right column Party Leader (highlighted) / Party Members / Team (Villains).
- Game Type and Map open a carousel popup over the dimmed page: current item large, next items
  small, a ">" arrow, and a one-line description under modes ("Shoot your enemies in the face.").

## Lobby names and values like the PS3 (2026-10-25)
- The PS3 kept every multiplayer text in text1 <lang>.networking (SID table: u32 count, then
  {u32 sid, u32 offset} pairs, UTF-8 strings), which the Collection lost. Tony's discs: the US
  extract has languages 2..18 without English/French, the European BCES00757 ISO has no French;
  the BCES00757DATA2 patch's text1/7.networking is English.
- net.bin map record +0 (u32) is the map title SID: stage The Stage 45206309, warzone The Plaza
  23480697, shambhala The Lost City 9ddf8b90, treasure The Temple 8f4a3a42, gompa The Sanctuary
  b5b2acd0, train-wreck The Train Wreck 3af91b54, ice-cave The Ice Cave 1dcca0f1, village The
  Village a036040e, the-fort The Fort d08e0327 (facility, ruins, highrise, museum: DLC).
- The catalogue labels are French translations of those titles (no official French found):
  Le Village, La Place, Le Sanctuaire, La Grotte de glace, La Cité perdue, Le Fort,
  L'Épave du train, Le Temple. Values follow the PS3 wording ("[A] kills" -> "50
  éliminations", "20 minutes", "Off" -> "Désactivé").
- The "Choisir..." entries are gone: a lobby opens on the PS3 custom game's choice, deathmatch
  on the first map, 50 kills, 20 minutes, 1 round, friendly fire off (UC2MP_SETTINGS_DEFAULT).
  Mode descriptions ("Shoot your enemies in the face." ee98504c) are in the same file.
- Checked offscreen: the custom game page shows those defaults.

## Classic maps in the native net game (2026-10-25)
- runtime_coop_mp.c's netinit only had the Fort for deathmatch: the competitive spawn choice
  (ChooseSpawn 0x644220: manager, slot -> spawner sid) and GetSpawnLocator (0x662fa0) read the
  online spawners of the net pak level only, and the Collection lost every "<map>-multi-pak".
- runtime_uc2mp_netmaps.c: for a competitive net game on a catalogue map, the main level stands in
  for the net pak (with the PS3 companion levels) and both functions answer from
  uc2mp_extracted_spawn_catalog.h (co-op spawners left out; the farthest from the others' last
  spawns). GetDeltaSeconds (0xa477c0) asserted for networked processes (+0x168 bit 8, Village's
  scripts): they get the frame's delta (0xa4785a je -> jmp).
- *net-games* deathmatch indices: Sanctuary 0, Ice Cave 5, Lost City 10, Train Wreck 15, Temple 20,
  Village 25, Plaza 30 (five variants each), Fort 124. UC2MP_NET_GAMES_LIST=1 prints the table.
- Checked offscreen, host + joiner on one PC: all 7 reach Playing (net mode 26) on PS3 spawners,
  no assert, no packet dropped; the Fort unchanged.
