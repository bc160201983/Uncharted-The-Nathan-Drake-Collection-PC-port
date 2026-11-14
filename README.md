# Uncharted: The Nathan Drake Collection — PC port

Run the PS4 **Uncharted: The Nathan Drake Collection** (CUSA02344: *Drake's Fortune*, *Among
Thieves* and *Drake's Deception*) natively on Windows. The game's own x86-64 code runs directly
on the PC; a runtime supplies the PS4 system libraries, and a Vulkan renderer executes the PS4 GPU
work.

By **BilalReloaded** ([YouTube](https://www.youtube.com/@BilalReloaded/videos),
[GitHub](https://github.com/bc160201983)), October 2026. What changed and when:
[CHANGELOG.md](CHANGELOG.md).

> **No game files are in this repository or its builds.** You need your own copy of the game
> (the dumped CUSA02344 folder). This project is not affiliated with Sony, Naughty Dog or
> Bluepoint Games.

## Status

| Game | Plays | Notes |
|---|---|---|
| Uncharted: Drake's Fortune | Yes | 60 FPS in the tested areas. Native rendering up to 2880x1620 (4K output upscaled from it, the HUD at 4K). |
| Uncharted 2: Among Thieves | Yes | Played at 1440p and 4K. 60 FPS and more; the heaviest view 99 FPS at 4K on an RTX 5080 (frame limit 120). |
| Uncharted 3: Drake's Deception | Yes | Played at 1080p and 1440p, 4K tested: the Cartagena crowd 90 FPS at 4K on an RTX 5080 (frame limit 120). |

The Collection menu, switching between the three games, saves, audio (music, speech, effects),
cutscene movies and controllers work. Details and open issues: [analysis/STATUS.md](analysis/STATUS.md);
plan and test list: [analysis/PLAN.md](analysis/PLAN.md).

## System requirements and the lowest PC that plays

- **Needs:** Windows 10 or 11 (64-bit); a CPU from **Intel's 4th-generation Core (Haswell, 2013)
  or newer, or any AMD Ryzen** (the games need AVX, F16C, BMI1, MOVBE and LZCNT); a graphics card
  or integrated GPU with **Vulkan 1.3** and a current driver; about **6 GB of free RAM** for the
  game (16 GB in the PC recommended).
- **Weakest GPU tested: the integrated AMD Radeon graphics of a Ryzen 7000/9000 desktop CPU**
  (2 compute units, about a quarter of a PS4's GPU). At **720p**: Uncharted 3 27-39 FPS,
  Uncharted 2 27 FPS, Drake's Fortune 24 FPS; at **540p** Uncharted 3 33 FPS. Playable, not
  smooth. Settings: preset **Low**, or resolution **1080p** with upscaling **FSR 1** at
  **Performance** (the game draws 960x540) or **Ultra Performance**, anti-aliasing off.
- **4 CPU cores:** 60 FPS in Drake's Fortune and Uncharted 3 (title screen and in play; a 4-core
  test with a fast GPU), with **Idle game threads: Sleep** (the default). On a 4-6 core CPU also
  try **Multithreaded rendering** (per game) and keep **Frames queued at 1**.
- **Players report:** Core i5-10400F + RTX 3050 6 GB plays Uncharted 2 and 3.

## Features

**Launcher** (`Uncharted Launcher.exe`)
- A card per game; settings are kept **per game**.
- Quality presets:

  | Preset | Resolution | Frame rate | For |
  |---|---|---|---|
  | Low | 1080p, FSR 1 Performance (draws 960x540) | 60 | weak GPUs |
  | Medium | 1080p (as on PS4) | 60 | most PCs |
  | High | 1440p | 60 | fast GPUs |
  | Ultra | 4K (Drake's Fortune draws 1620p, upscaled) | up to 120 (experimental) | very fast GPUs |

  Every preset queues 1 frame. 2 frames queued is faster in heavy scenes, but GPU results
  reach the game a frame later: keep it at 1.

  **Low-end PCs and laptops:** keep **Idle game threads** (launcher, **Performance** tab) on
  **Sleep** (the default). With **Spin** the games' six job threads keep their cores busy while
  they wait for work, as on the PS4 where each has its own core; on a 4-6 core CPU that took
  nearly all of it (a 4-core test: 19-27 FPS on the title screen, 60 with Sleep). Also turn on **Multithreaded rendering** (per game) and keep **Frames queued at 1**. The PS4's
  GPU commands are otherwise processed on one CPU core, which limits slower CPUs; with this option
  the Vulkan command recording runs on a second core. On fast desktop CPUs it makes no difference.
  If a game hangs while loading, turn that option off again. With a weak GPU also pick the **Low**
  preset (FSR 1 Performance at 1080p); integrated GPUs: FSR 1 **Ultra Performance**.

- **Display:** windowed, borderless or exclusive fullscreen; resolution 540p, 720p, 900p, 1080p,
  1440p, 1620p or 4K; aspect ratio 16:10, 16:9, 21:9, 32:9; **upscaling** FSR 1, FSR 3.1, FSR 4
  or NVIDIA DLSS: the game draws below the resolution (Quality 1/1.5, Balanced 1/1.7, Performance
  1/2, Ultra Performance 1/3 of its width and height) and each frame is upscaled to it before the
  HUD and menus, which stay sharp; FSR 3.1, FSR 4 and DLSS also at the resolution (Native AA,
  DLAA); anti-aliasing TAA (with upscaling off or FSR 1); RCAS sharpening; FPS counter. Upscaling
  saves GPU time only: where the CPU limits the frame rate (most PCs at 1080p and 1440p, Steam
  Deck) it changes little.
- **Performance:** frame rate limit 30 / 40 / 45 / 50 / 55 / 60 / 75 / 90 / 100 / 120 (60 is the
  PS4's; above 60 is experimental, and faster than 120 breaks the games' movement, so it is not
  offered; with VSync a limit at or above the monitor's refresh rate follows the monitor); frames
  queued; background shader compilation; multithreaded rendering (experimental); idle game
  threads (sleep or spin).
- **Keyboard and mouse:** mouse look (sensitivity, invert Y), right mouse aim, left mouse fire, WASD,
  and every action remappable per game in the **Controls** tab (two bindings per action, keys or
  mouse buttons). Controllers (DualSense, DualShock 4, Xbox) work at the same time.
- **Language** (Advanced tab): any of the 22 languages the games have text for; each game's main
  menu then offers it next to English (Square). Voices: the languages in your copy of the game.
- **Unlock all chapters** (Advanced tab, off by default): every chapter and the Brutal difficulty
  from the start in all three games; saves are untouched.
- **Game update 1.02** is used automatically when it sits next to the game folder (see below).
- Log and save folders, desktop shortcut, shader cache reset, game folder selection, links to the
  author's [YouTube](https://www.youtube.com/@BilalReloaded/videos) and GitHub.

**In game:** `Insert` or `LT + R3` opens the menu (display, anti-aliasing, draw distance, frame rate,
overlay, controls). Both shortcuts can be changed in its Controls tab (any key; any one or two
controller buttons).

**Under the hood**
- Native execution of the three game executables; the Collection's game switching (LoadExec).
- shadPS4-derived Vulkan renderer with a persistent shader/pipeline cache, asynchronous shader
  compilation, and fixes for this game: GPU-written constants synced back to the CPU, MUBUF
  ADDR64 loads (character shadows), direct memory access for the tiled lighting, depth/stencil
  aliasing, CPU writes next to GPU data without stalls, GPU ray-cast results returned to the game
  (Drake's Fortune's bullet impacts, blood and decals), tolerant handling of garbage descriptors.
- Render resolution patch: the games' display settings and memory map are rewritten for the chosen
  resolution; cutscene movies are scaled to it and their conversion shader adjusted.
- FFmpeg-based video decoding (Videodec2), ATRAC9 audio (LibAtrac9), SDL3 window and controllers.

## Known issues

- **Drake's Fortune:** the grenade launcher can kill the player (GitHub #6). Being looked at.
- **Slower CPUs:** the PS4's GPU commands are processed on one CPU core (3,000-5,600 draws per frame
  in the heaviest scenes); on older CPUs those scenes can drop below 60 FPS.
- **Multithreaded rendering is experimental:** its crashes with 1 frame queued are fixed. Drake's
  Fortune always runs without it (its guns flickered grey with it, GitHub #23). It is
  meant for low-end PCs and laptops (see the presets above) and gives no speed-up on fast CPUs;
  with "Frames queued 2" Uncharted 2 can still stop with the game assert `m_gfxEopTick`, so keep 1.
- **Recording with OBS Game Capture:** if OBS keeps losing and re-hooking the game, the game's
  memory use grows (each hook creates DirectX devices in the game). With an NVIDIA card set
  NVIDIA Control Panel -> Manage 3D settings -> Program settings -> Uncharted.exe ->
  **Vulkan/OpenGL present method: Prefer native**, or use OBS **Window Capture**.
- **Frames queued 2** is faster in heavy scenes, but GPU results reach the game a frame later
  (Uncharted 2's NPCs float, Nate's feet sink into the floor): keep it at 1 (every preset does).
- Drake's Fortune renders at most 2880x1620 (its memory layout); above it that size is upscaled
  to the resolution before the HUD (FSR 1, or the temporal upscaler chosen).
- Frame rates above 120 are not offered: the games' movement breaks (Drake's Fortune at 160 FPS
  dropped Nate through the world).
- Uncharted 3 has had less play testing than the other two: please report crashes with the log
  (launcher **Advanced -> Open log folder**, `boot.log`).

## Playing

1. Download **Uncharted-windows.zip** from the latest release on the
   [Releases](../../releases) page (or a newer build from the
   [Actions](../../actions/workflows/build-windows.yml) tab, latest successful run → Artifacts),
   or build it yourself (below). Extract it anywhere.
2. Start **Uncharted Launcher.exe**. On the first Play, select your game folder: the dumped
   `CUSA02344` folder (the one holding `eboot.bin` and `sce_sys`, often called `uroot`).
3. **Recommended: update 1.02** (Bluepoint's stability, movie, audio and gameplay fixes). Extract
   the update next to the game folder, named like it plus `-patch` (for example `CUSA02344-patch`,
   with or without its `uroot` folder). The launcher's Advanced tab shows the version in use.
4. Pick a game and a preset, press **PLAY**. The first start of each game prepares files and
   compiles shaders: expect a slow first load and some stutter until the cache has filled.

**Requirements:** Windows 10/11 64-bit, a GPU with Vulkan 1.3 and a current driver (NVIDIA, AMD,
Intel), 16 GB RAM recommended, a 4-core CPU or better. Saves, logs and settings stay in the
`analysis` folder next to the launcher.

**Updates** (from version 0.1.1): the launcher checks the releases when it starts and shows
**UPDATE TO x.y.z** when there is a newer one. It downloads the package, checks its SHA-256 against
the release, replaces the program files (saves, settings and the shader cache stay) and starts
again; if anything fails, the installed version is put back. Advanced tab: turn the check off, or
check now.

## Building

GitHub Actions builds every push to `main` ([build-windows.yml](.github/workflows/build-windows.yml)).
To build locally, from an MSYS2 **CLANG64** shell:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
git clone --recursive https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port.git
cd Uncharted-The-Nathan-Drake-Collection-PC-port
gow3_pc/build.sh                 # the runtime: gow3_pc/out/gow3-probe.exe
tools/package_windows.sh         # dist/Uncharted-windows(.zip); needs a Windows Python 3.10+
                                 # with Pillow (PYTHON_HOME)
```

To run from the source tree: `Start-Uncharted.cmd` (launcher), or
`python tools/probe_uncharted.py --boot --seconds 0 --game <game folder>`.

### Releases

`tools/release.sh 0.1.1` publishes version 0.1.1: write its text in
`tools/release-notes/v0.1.1.md` first. The script sets `tools/VERSION`, pushes the source and the
tag `v0.1.1`; GitHub Actions then builds the package and publishes the release with
`Uncharted-windows.zip` (about 20 minutes). The launcher offers newer releases as updates.

## Repository layout

| Path | Contents |
|---|---|
| `launcher/` | The Uncharted launcher (Tkinter + Pillow). |
| `tools/probe_uncharted.py` | Prepares a game executable (patches, render resolution, update) and starts the runtime. |
| `tools/package_windows.sh` | Builds the Windows package; `tools/windows_starter/` its `.exe`. |
| `tools/release.sh`, `tools/release-notes/` | Publishes a release (see above). |
| `gow3_pc/` | The runtime: C runtime (`src/`), GPU library (`gpu/`: shadPS4 video core and shims), preparation scripts (`scripts/`). |
| `analysis/` | Compatibility notes (`STATUS.md`) and the plan (`PLAN.md`). |

## Credits and license

- **Uncharted port, launcher and game fixes: BilalReloaded**
  ([YouTube](https://www.youtube.com/@BilalReloaded/videos),
  [GitHub](https://github.com/bc160201983)), 2026. The fixes and their dates are listed in
  [CHANGELOG.md](CHANGELOG.md); the commit history shows each change.
- The runtime started from the [gow3_pc](https://github.com/FelipePrado0/gow3_pc) project
  (whose internal names, such as `GOW3_*` settings, it keeps) and the
  [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.
- Temporal anti-aliasing for Uncharted takes the camera from the games' motion-blur velocity
  pass as SuperMedo's [Uncharted port](https://github.com/Supermedo/uncharted_pc) does (GPL v2;
  `CameraMotion::OnVelocityConstants` and the motion vector mode come from it).
- **NVIDIA DLSS:** DLAA uses NVIDIA DLSS technology: the NVIDIA DLSS SDK 310.9.16
  (`nvngx_dlss.dll`, under NVIDIA's license, in `licenses/nvidia-dlss`) through the DLSS bridge adapted from
  [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) (MIT).
  NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.
- AMD FSR 4: the v07 INT8 model as built by [Q2RTX](https://github.com/FireBurn/Q2RTX) from AMD's
  MIT-licensed FidelityFX SDK source.
- Third-party code: FSR (AMD), imgui, magic_enum, miniz, xbyak, LibAtrac9, FFmpeg, SDL3.
- Licensed under the GNU GPL v2 or later (see `gow3_pc/LICENSE`). The Windows package carries
  `LICENSE.txt` and a `licenses` folder with the license of every bundled library
  (`licenses/THIRD-PARTY.txt` lists each DLL package with its version, license and source).

### Reusing this work

You are welcome to use these fixes in your own port. The GPL asks that you keep the copyright and
license notices and publish your source code under the same license when you distribute your
build. Beyond that, a request: **credit this project and link back to it**
(`https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port`, by BilalReloaded) in your README or release
notes, so people can find where a fix came from.
