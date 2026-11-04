# u3mp — restoring Uncharted 3 multiplayer (LAN) in the PC port

Work in progress on bringing back Uncharted 3: Drake's Deception multiplayer, which Bluepoint
disabled in the PS4 Collection, on top of this port. The goal is to replicate the PS3 game
(BCES01670, patch 1.19 + DLC) faithfully: same code paths, same ids and strings, porting what
Bluepoint stubbed out rather than inventing replacements.

> **No game files are included.** Everything here is source code, scripts and notes. The tools
> read your own dumps. Images and assets they produce (`builds/`, `work/`, converted paks) are
> derived from the game and stay out of git (see `.gitignore`).
>
> **Region:** every PS4 address in this folder was found on the **CUSA02320** (EU) U3 executable
> (U3 1.02). The README of this repository targets CUSA02344; the offsets have not been checked
> against that build yet.

## Status (October 2026)

| Area | State |
|---|---|
| Multiplayer menu, LAN Party, LAN profiles (create/rename/delete, saved to `user/lan-profiles.v1`) | Working |
| LAN lobby (Custom Game, Find a Party, Profile, Options, party list) | Working |
| Custom Game: game types, maps, rules | Working (7 modes, 11 maps on PS4 data) |
| Start Game → match states → in game (net state 0x1d) | Reached with a solo host; City of Brass loads |
| Player character spawn in a match | Not yet (spawner/character creation still blocked; see docs) |
| Two PCs (real LAN transport) | Not wired yet: the PS3 LAN backend class is stood in by a guest stub |
| Lobby backdrop stage (`san-upper-multi-stage`) | Loads (v4.61/4.62); lobby camera/character placement being restored (v4.63, untested) |
| PS3 → PS4 asset conversion | Characters convert and render in game (PS3 Drake with PS4 remastered textures); map stages/levels next |

The full findings log, with every disabled piece and how it is restored, is
[docs/u3-lan-restoration-findings.md](docs/u3-lan-restoration-findings.md). Mesh, texture and pak
formats (PS3 and PS4) are in [docs/PAK-AND-MESH-FORMATS.md](docs/PAK-AND-MESH-FORMATS.md).
Note: older notes call map 4 (`san-upper-multi`) "Sanctuary". It is **City of Brass**.

## Layout

| Folder | What |
|---|---|
| `helper/` | `u3_lan_profiles.c`: freestanding C helper linked into the game image (LAN profiles, lobby lists, session backend stand-in, match start, lobby stage/camera). `build_u3_*.py` patch it into a prepared image; `build_lan_lobby.py` does the whole build in one step. `tests/` has host-side unit tests. |
| `runner/` | `run_u3_probe.py`: launches the port with a prepared image in an isolated test folder, never touching the originals. Page-aware scripted navigation (`--nav`), screenshots, file trace, net/state trace and guest memory watches (`--peek`). `Test-*.cmd` are the launchers; `Test-Watcher.cmd` runs queued test jobs unattended; `nav/` holds navigation scripts; `shots.py` converts frame dumps to PNG. |
| `paktools/` | Naughty Dog pak/DC readers and converters: `ndpak.py` (PS3 and PS4 paks), `dc.py` (DC script data), `ps3geom.py` (PS3 foreground/background mesh decoder, verified against PS4 rosetta pairs), `ps4actor.py` (PS3 actor → PS4 pak converter), `ps4writer.py`, `texcodec.py`, `convert_net.py` (net.bin), inventory scripts, `matsurvey.py` (PS3↔PS4 material survey). |
| `re/` | Reverse-engineering helpers: Ghidra scripts (`ghidra/` for the PS4 build, `ps3/` for the PS3 eboot) and the Python analyses behind the findings (menu ownership, task graphs, PS3 backend, PS3↔PS4 function matching). `re/data/` holds derived address maps (no game data). |
| `docs/` | Findings and design notes. |

## Setup

1. A working install of this port, and your own dumps: the PS4 game folder (CUSA02320 used here)
   and, for asset porting, the PS3 game (BCES01670) with its patch and DLC packages.
2. Environment:
   - `UNCHARTED_GAME_DIR`: the PS4 game folder (contains `eboot.bin` and `u3data/`).
   - `UNCHARTED_PC_DIR`: the port install. Defaults to the repository root (`gow3_pc/out/Uncharted.exe`,
     `python/python.exe`, `analysis/big3-ps4_Shipping`).
   - The `re/` scripts also read `U3MP_PROJECT_DIR`, `U3_PS3_GAME_DIR` and `U3MP_WORK_DIR` for their inputs.
3. Helper blobs: `python helper/build_u3_pool_blob.py --source helper/<name>.c --out work/<name>.bin`
   for `u3_pool_init`, `u3_menu_entry` and `u3_menu_flow` (see docs/MULTIPLAYER-MENU.md and
   docs/POOL-BOOTSTRAP.md).
4. Base image: run a `--mode menu-flow` test with `runner/run_u3_probe.py` and freeze it with
   `helper/freeze_u3_menu_build.py <run folder> builds/menu-shell-v1` (the build used here has
   SHA-256 `8979cc0d…554b`; docs/SESSION-STARTUP.md).
5. LAN lobby image: `python helper/build_lan_lobby.py 4.63` (needs clang, MSYS2 clang64 by default).
6. Run `runner/Test-Uncharted3-LAN-Lobby.cmd` (set `U3MP_BUILD` to pick a build).
   In game: Multiplayer → LAN Party → choose a profile.

Converted assets go into an overlay folder passed with `--overlay` (read-only app0 overlay, so the
game folder is never modified), for example `work/overlay-mp/u3data/build/main/actor33/ps4/`.

## Unattended testing

`runner/Test-Watcher.cmd` waits for `work/queue/job.cmd` and runs it. A job is a normal
`run_u3_probe.py` command with `--nav runner/nav/nav-stage.json --exclusive-script`, which walks
title → Multiplayer → (offline dialog if shown, via `if_page`) → LAN Party → profile → lobby and
captures frames. `--peek name=OFFSET[>OFFSET...]:SIZE` logs guest memory (image-relative offsets,
`>` dereferences a pointer) to `peek.jsonl` whenever it changes.

## How the helper hooks in

The helper is a freestanding blob placed after the image with a private state page
(`ProfileState`). Native PS4 functions it calls are listed in `helper/build_u3_profile_probe.py`
(`NATIVES`) and named in `u3_lan_profiles.c` (`NATIVE_*`). Version history and the reasoning for each
patch are in the findings log. The latest additions:

- **v4.60–4.61:** lobby backdrop. The PS4 still ships the City of Brass lobby stage
  (`san-upper-multi-stage` + `sky-san-upper-multi`). The game's own stage loader 0x743420 runs when
  a map is picked in Custom Game; the helper also requests the stage when the LAN lobby opens.
- **v4.62:** calls the orphaned menu-scene entry 0x5b7020 (MenuScene 0x17bba90 state 2 + MP loadout
  packages). Bluepoint removed every call to it.
- **v4.63:** requests `anim-proto-multiplayer` (the scene waits for it before placing the lobby
  character at the stage's `menu-spawner-1`), records the stage name at netgame+0x74b8 like the
  native loader, and calls the orphaned lobby camera setup 0x5b4e60 (PS3 called it from its
  frontend loop) once the stage is loaded.
