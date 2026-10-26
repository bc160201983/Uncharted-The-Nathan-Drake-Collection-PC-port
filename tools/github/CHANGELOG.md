# Changelog

Uncharted: The Nathan Drake Collection PC port, by **BilalReloaded**
([YouTube](https://www.youtube.com/@BilalReloaded/videos), [GitHub](https://github.com/bc160201983)).
Technical details of every item: [analysis/STATUS.md](analysis/STATUS.md).

## 26 October 2026 (0.7.0)

- **Uncharted 3 washed out / too bright on GeForce RTX 30 and 40 cards** (#199, #195, #204, #212):
  the game's automatic exposure adds up each row's brightness in a compute shader whose last six
  steps have no barriers: they rely on the PS4 GPU running 64 threads in lockstep. These cards run
  32 at a time, so the sums came out too small (1056 instead of 2616 on the title screen) and the
  exposure about 2.2 times too high. The shader (aec539f0) now gets the barriers on every GPU,
  like Uncharted 3's cloth shaders; only that shader is compiled again. The title
  screen's mean brightness on an affected laptop went from 205 to 166, the same as on other cards.
- **Uncharted 3: no subtitles over movies with upscaling** (#237, #243, #222): every movie frame is
  one compute pass that writes the screen from the decoded picture and the subtitle layer. With no
  normal frame in between, the upscaler's interface phase of the last game or menu frame stayed on
  during the whole movie, so the subtitles went to an image the movie never read. A compute pass
  that writes the screen now ends that phase, and the movie is shown as the game drew it.
- **AMD: crash at start in the shader warm-up** (#230, #201, #206, #185; the driver crashed at the
  same place for everyone): a saved pipeline named its shaders by program and variant number, and
  the motion-vector variants could be paired with a different shader on the next start. Saved
  pipelines now record exactly which shaders they were built from and are skipped when those don't
  match. A crash while the saved shaders load is also noted, and the next start clears and rebuilds
  the cache by itself.
- **Chapter 8 crash on game version 1.00** (`m_lookAt.X() == m_lookAt.X()`, #148, likely #210): the
  camera focus fix made for 1.02 in 0.2 now has 1.00 variants for Uncharted 2 and 3.
  UNCHARTED_FOCUS_RESET=0 turns it off.
- **1.00 executables with a 1.02 version file** (#148, #176): some copies have update 1.02's
  param.sfo but the 1.00 game executables, so the launcher said 1.02 and the 1.02 fixes were
  skipped. Such a copy is now reported as 1.00, and the "install update 1.02" warning shows.
- **Launcher: Advanced -> Uncharted 3 exposure**: an optional, lower exposure for Uncharted 3
  (-0.3, -0.6 or -1.0 EV; "Game's" = unchanged), based on pavlikdcx's patch in #199.
- **Launcher: IGPU preset** for Intel and AMD integrated graphics (540-720p, FSR 1 to 1080p),
  chosen as the recommended preset when the game ran on integrated graphics; adapted from PR #223
  by memeguyoffical.
- **Launcher: Add to Steam** (Advanced -> STEAM): adds the port (or each game) to Steam's library
  as a non-Steam game, with library pictures from the player's own game files. Steam's
  shortcuts.vdf is read and written losslessly, with a backup; Steam must be closed.
- **Merged pull requests:** #240 (Windows test builds and test expectations) and #227 (the package
  zip is written with Python's zipfile, forward-slash paths) by moistman7; #231 by Sparks and #244
  by fidalgojoao (research and code that is off in this release).

## 25 October 2026 (0.6.2)

- **The launcher did not open after updating to 0.6.1** (#208): the launcher's new network search
  for the coming Uncharted 2 multiplayer is in a separate file, `launcher/net_lan.py`, which the
  launcher loaded at start. The package build copied only the launcher's two older files, so 0.6.1
  shipped without it and the launcher stopped before its window appeared. The package now copies
  every launcher file, and the launcher opens even when that optional file is missing.

## 25 October 2026 (0.6.1)

- **Fewer random crashes in all three games** (crashes like "execute access to 0" or a sudden
  close to the desktop, possibly also #205): the games' PS4 code keeps some values in the 128
  bytes just below the stack pointer, a space the PS4's calling rules leave alone. Windows does
  not: when it handles a page fault (the port uses them to track memory the GPU writes) it builds
  its exception record right there and can overwrite those values. We had fixed two such routines
  by hand (the shader constant tables in 0.4.1, the particle grouping in 0.6.0). Now the port finds
  every routine that keeps values below the stack when a game starts (162 in Drake's Fortune, 144
  in Uncharted 2, 153 in Uncharted 3) and runs each of their memory accesses that can fault with
  the stack moved out of the way first. In a test that overwrites that space after every fault,
  as Windows 10 does, the games now run instead of crashing at start, and the frame rate is
  unchanged (Uncharted 2 uncapped: 96.6 vs 96.8 FPS). GOW3_RED_ZONE=0 turns it off.
- **Drake's Fortune: mouse aiming without the "stick" feel** (#190): the aim camera's remembered
  direction is now turned directly by the mouse, as in Uncharted 2 and 3 since 0.6.0: the same
  movement turns the same angle, slow or fast (about 260 degrees for a fast swipe in our test, as
  in Uncharted 2), with no speed cap and no acceleration. The game makes new aim cameras during
  one press (after a roll), and each gets every mouse turn exactly once.
- **RivaTuner's on-screen display flickered** (#127, #128): since 0.5.0 the game presents its frames
  on a queue of its own, and RivaTuner missed some of them (its overlay was missing from 5 of 60
  captures in Uncharted 3 with DLSS at a 120 FPS limit). With RivaTuner running, frames are
  presented on the graphics queue again (60 of 60).
- **Uncharted 2 ultrawide: shadows at the sides of the screen** came and went as the camera turned
  at 21:9 and 32:9: the sun, rain and lamp shadows decided what casts a shadow with the 16:9 view.
  They now use the full width (thanks to Ferosnow95, #192).

## 24 October 2026 (0.6.0)

- **Uncharted 3 too bright in sunny areas** (#140): the glow around bright light (bloom) was
  drawn into a separate image the game never read, so the final picture used stale data from
  memory instead, a haze that washed out sunlit places like Cartagena and the Chateau. The bloom
  now goes into the right texture and shows as a soft glow.
- **Mouse aiming and camera without the "stick" feel** (#190, #86, #141, #158, #170): aiming in
  Uncharted 2 and 3, and the camera in Drake's Fortune, now turn directly from the mouse instead
  of through an emulated right stick, which had a speed cap and a ramp that felt like
  acceleration. The same mouse movement now turns the same angle, slow or fast (about 63 degrees
  per 1,000 counts at sensitivity 1). Windows' pointer acceleration is kept out, and the
  sensitivity goes up to 5x in the launcher (10x in the in-game menu).
- **Uncharted 2: the most common crash, "read access to 0 at guest offset 0xd09665"** (#61 #113
  #115 #164 #181 #187; often around trains, where particle effects are heavy): a particle routine
  kept two loop values just below the stack pointer. When Windows handled a page fault in the
  middle of it, it wrote over one of them, and the loop read one entry past the end of its list.
  The routine now takes those values from registers it never changes (a 10-byte patch, also in
  Uncharted 1 and 3, which share the routine).
- **Save profiles** (#66): launcher Advanced -> Save profile gives each game three separate
  save profiles, each with its own progress and in-game settings, like separate PS4 users.
  Profile 1 holds the saves from earlier versions; switching never moves or deletes a save.
- **Controller vibration on/off** (#166): launcher Controls and the in-game Controls tab.
- **Games that stopped at start with update 1.02 in a separate folder** ("files.json -> not
  found", "ASSERTION: root"; #43 #73 #139 #151 #153 #165 and others): where Windows allows
  symbolic links (Developer Mode, a launcher run as administrator), the game read the update's
  linked archives as empty files. The port now reads a linked file's real size, and the update
  folder links files as hard links first.
- **Updating keeps your own files** (#134): files you put into the port's folders (ReShade,
  notes, a game folder) are no longer removed by the launcher's update.
- **The update download continues after a stall** (#150) instead of failing.
- **Mouse side buttons can be bound in the launcher** (#152).
- A crash message now says when the game is version 1.00 without update 1.02.
- Device-lost crash logs now include the graphics driver's fault report.
- **Half the RAM and faster shader loading** (#186): the port no longer saves "pipeline binaries"
  (about 2 MB for each of thousands of shader pipelines). Each pipeline loaded from one kept its
  2 MB in memory: Uncharted 2 used 15.5 GB of RAM with them and 7.0 GB without, and shader
  loading at the start took 40 s instead of about 10 s. The graphics driver's own shader cache
  does the same job. The old folder (up to 20 GB) is deleted at the first start.
- **Multi-second shader-compile freezes that 0.5.0 still had** (#144, #132, #142, #117): one
  effect shader still built a new version for every broken buffer slot whose address is 0 or
  whose size wraps to 0. Those slots now count as empty, as they are drawn.
- **Uncharted 2: crash on a garbage texture with an unknown format** (#144, "pixel_format.cpp
  NumBitsPerBlock"): such textures are now skipped like other broken ones.
- **Temperatures in the performance counter** (#178, thanks to memeguyoffical): the graphics
  card's temperature (NVIDIA, from its driver) and the processor's where Windows reports one,
  next to their use. RAM shows the game's own memory.
- **The performance counter's settings in the launcher** (#179, thanks to memeguyoffical):
  Display tab, what it shows, its corner, layout, size and background.

## 21 October 2026 (0.5.0)

- **Wrong-looking surfaces from a damaged shader cache are repaired**: in Uncharted 3's museum the
  tiled floor of one room showed as a flat red surface on one PC and correctly on another. The
  shader cache keeps each shader version as two files, a description and the compiled code. A
  session had rewritten the code file while Windows refused to replace its description (another
  thread was reading it), so every later start drew that floor with another version's code.
  Descriptions now carry a checksum of their code; a pair that doesn't match (or, in caches from
  earlier versions, whose files were not written together) is thrown away and compiled again
  once. On the affected PC exactly one pair out of about 20,000 was damaged, and the floor now
  shows its pattern.
- **Stutter when the game streams in the next area is gone on most laptops and iGPUs**: when a
  level part loads into memory the GPU had used, the game rewrites it, and on graphics cards whose
  memory the CPU can read directly (Resizable BAR, most current laptops; integrated GPUs) the port
  fetched the GPU's bytes one 4 KiB page per write fault: thousands of faults in a few seconds,
  50-180 ms frames (a laptop in Uncharted 3's Cartagena: 31-46 frames over 40 ms in two
  minutes). Each fault now fetches the finished GPU pages of its 256 KiB window in one copy, and
  the rest of the window is written without faults: 0-4 such frames in the same spot, none from
  this. Drake's Fortune had the same stalls.
- **Multithreaded rendering no longer makes objects flicker**: with it on, whole objects drew
  flat white for single frames in Uncharted 2 (a train car, Drake's gun, enemies; 6-13 times in
  10 seconds of combat on a busy CPU) and weapons flat grey in Drake's Fortune (#23). With it on,
  the port does a small constant copy of the game's on a second thread, and the next draws could
  read its destination before it was written. Those draws now wait for that copy (a few dozen
  times a second at most). Measured: 0 flickering frames in all three games, no FPS cost.
- **Drake's Deception: multithreaded rendering on by default**: the port had kept it off in
  Uncharted 3 since the freeze of #51 (fixed in 0.4.0). In a heavy Cartagena spot on a
  laptop, where one CPU thread prepares every draw, it is the difference between 53.7 FPS with 11
  stalls over 100 ms (off) and 60 FPS with 1 (on); no freeze in 15-20 minute tests. The launcher
  turns it on once for Uncharted 3 (Advanced -> Multithreaded rendering turns it off again).
- **Shader cache kept per graphics card**: a laptop that runs the game on its integrated GPU one
  time and on its graphics card the next lost the whole shader cache at every switch (4,478 files
  on a laptop, then half-second compile freezes for minutes). The other card's cache now
  waits beside the current one and comes back when you switch back (at most two are kept; if an
  older version ran in between and left two caches of one card, the newer one comes back).
- **Keyboard prompt for Uncharted 3's d-pad picture**: the d-pad prompt with its up arm lit
  (young Drake in the museum, and wherever the game shows it) still showed the PlayStation
  d-pad with the keyboard and mouse. It now shows the key bound to d-pad up on the lit arm.
- **Aiming blur above 1080p is gone again (Uncharted 2 and 3)**: when you aim, the game measures
  the distance under the crosshair to set its depth of field. Above 1080p the port moves that
  measuring spot to the crosshair, but the corrected position sometimes didn't reach the GPU, so
  the focus kept jumping between your target and the wall beside Nate (blur while aiming). The
  correction now always reaches the GPU (checked at 4K: every frame corrected).
- **Warning for USB, SD and exFAT drives**: with the port and the game on an external USB drive
  (an NVMe SSD in a USB case, exFAT), a laptop froze for 0.2-0.8 s dozens of times a
  minute, because Windows writes to such drives without a cache and the port writes its shader
  cache, logs and saves while you play. On the laptop's internal drive the same test had no
  freeze over 100 ms. The launcher now says so before you play from such a drive (once per
  folder), so you can copy the port and the game to an internal SSD.
- **Drake's Fortune: the most common crash, "execute access to 0"** (#27, #30, #42, #62; the
  same code is in Uncharted 2 and 3): for each shader the game fills a small table in a function
  that keeps its counters just below the stack pointer, a place Windows' exception handling does
  not have to keep. When the table crossed into a page the port watches for the GPU, the port's
  page fault landed inside that function, a counter came back wrong, and a later draw called an
  empty slot. The table's pages are now written once before the function starts, so a page
  fault happens there, where nothing can be lost. It depends on the Windows version, which is why
  it hit some players often and others never.
- **Fewer multi-second freezes in all three games** (#71, #79): an effect and crowd-skinning
  compute shader the three games share reads some of its buffer slots from floats when an effect
  doesn't use them. The port already bound those as empty buffers, but it still built a separate
  version of the shader for every new set of float values, compiled in the middle of the frame:
  one shader had collected 1,177 versions, and single frames took up to 7.8 s here and 10 s on a
  laptop. Those slots now count as empty when the shader version is chosen, so nothing new is
  built for them. The same bug made memory rise through a session (#95): a 0.4.1 log shows
  Uncharted 2 holding 9,942 compute pipelines after a few minutes, where the port now holds
  about 30. A shader that still builds 100 or more versions is now named in the log.
- **Faster start, and no more crash while loading shaders, for players of earlier versions**
  (#92, #95): the shader cache had kept every version those freezes built, and the loading at
  start rebuilt them all each time: 23,845 compute shaders in one player's cache (the AMD driver
  crashed near the end of loading them), a 332-second load in another. None of those versions is
  used again. At the first start of this version, a shader with 100 or more cached versions is
  left out of the loading and its files are removed (the log names it); the one or two versions
  the game really needs are built again when first used. On our PC that removed 1,651 of 1,730
  compute shaders; all three games then run with 82, and the next start finds nothing to remove.
- **Uncharted 2: crash on a garbage texture fixed** ("image_info.cpp UpdateSize: Assertion
  Failed"): the game sometimes points a draw at data that isn't a texture. One such description,
  a compressed format with a tiling layout compressed textures never use, made the port stop
  while working out its size, before the check that skips garbage textures. It now has no size
  and is skipped like the others. Found in a 16-minute session on a laptop. A test
  that turned 3,000 textures into that one reproduced the crash on the previous build; this
  version kept running at 60 FPS. The same test found a second such stop (a garbage texture read
  as a depth texture), fixed the same way.
- **Video memory no longer only grows** (#95): the port kept every buffer range the game ever
  used in video memory. On a laptop, 16 minutes of Uncharted 2 with one level change
  took that from 320 MiB to 1.9 GB, never less, and through a long session cards with 4-8 GB
  fill up (then the driver moves memory to system RAM and the frame rate falls). Now, above a
  quarter of the card's memory, ranges the game has not used for a minute are given back and
  that memory is used again first. Ranges holding data only the GPU has are never given back.
- **Less "out of device memory" on cards with little video memory** (#81): once the card's own
  memory was full, the driver refused more, and the port stopped. On drivers that support it
  (NVIDIA's do), the driver may now move memory to system RAM instead: slower while the card is
  full, but the game goes on. (Checked with the driver; not yet on a small card itself.)
- **5.1 and 7.1 surround sound**: the three games have their own stereo, 5.1 and 7.1 mixes,
  and pick one from the speaker setup the system reports. The port always reported stereo, so
  only the front left and right speakers ever had sound. It now reports your Windows output
  device's setup (launcher Advanced -> Speakers: Auto, or Stereo / 5.1 / 7.1): on a 5.1 or 7.1
  system the centre, subwoofer and surround speakers play the games' surround mix. Stereo
  outputs and headphones keep the games' own stereo mix (a 5.1/7.1 mix chosen by hand is folded
  to stereo there).
- **Unlock all rewards** (launcher Advanced, off by default): every reward in Bonuses (skins,
  weapons, render modes, art galleries, gameplay tweaks such as Slow Motion, Mirror World or One-
  Shot Kills) and every story cutscene, in all three games, and rewards that are normally enabled
  only after finishing the game can be enabled right away. Uncharted 2 and 3 use Naughty Dog's own
  "give all" switches; Drake's Fortune's reward checks are patched. Nothing is written to the save:
  turned off, the menus show your own progress again. Treasures, medals and trophies are not
  touched. Needs update 1.02.
- **Trophies**: the games award trophies as on the PS4, but the port only wrote them to the log
  and told the games at every start that none had been earned. Earned trophies are now saved
  (in the user folder, trophy/), the games are told which you have, and a new one shows a
  PS5-style popup in the top right corner (below the FPS counter when it is there) with the
  trophy's picture from your copy of the game and a short chime. The in-game menu (Insert) has a
  Trophies tab with every trophy of the running game, earned ones lit. The trophy names are
  encrypted in the game files and are not shown. Trophies earned with earlier versions were not
  saved, so the count starts at zero.
- **Controller light bar**: the games set the DualShock 4 / DualSense light bar (blue at the
  start, then their in-game colours; Uncharted 2 also turns it red). The port ignored that, so
  the light bar stayed as the driver left it. It now shows the games' colours.
- **Smoother mouse aiming in all three games**: while aiming, Uncharted 2 and 3 turn their aim
  camera by the right stick, but the mouse was mostly sent to the normal camera then, so aiming
  with the mouse was choppy. While aiming the mouse is now the stick, and its speed is converted
  with the inverse of the games' stick response, so the turn follows the mouse evenly (slow
  movements turned too little before, fast ones too much). New mouse options (in-game menu
  Controls tab and the launcher): aim sensitivity, up/down speed and invert left/right.
- **Memory no longer grows the whole time you play**: each nested GPU command list kept a small
  piece of memory forever, about 140 MB a minute in Uncharted 3 (over 8 GB in an hour of play,
  enough to run a 16 GB PC out of memory). In the same test, memory now stays flat.

- **US version (CUSA02320) supported**: tested from the US PKGs (base and update 1.02): Uncharted 1,
  2 and 3 start and play (New Game in each, 60 FPS). Its 1.02 executables are byte-identical to the
  European ones (CUSA02344), so every fix and patch of the port applies. Install it with the new
  Install game from .pkg below. If your US copy stopped at the start before (#29, #43, #58, #73),
  please try this version and send launcher-last-run.log if it still does.
- **Install the game from its .pkg** (launcher Advanced -> FOLDERS -> Install game from .pkg): for
  players who have the game as a PKG file (for example the US version, CUSA02320) instead of an
  extracted folder. It unpacks it into a folder you choose, with the files the launcher and the
  update installer need (unpacked by hand, those were missing and the update could not be
  installed), and makes it the game folder. Then install update 1.02 with GAME UPDATE -> Install
  from .pkg. The US version's game files are the same as the European ones, so every fix applies.

From kuraeow (Steam Deck), merged:

- **Games without update 1.02: idle game threads wake up for new work again** (#87): the fix that
  lets the game's idle worker threads sleep instead of using whole processor cores has three
  parts. On version 1.00 the code is compiled with other registers, so only the part that puts
  the threads to sleep was applied: they woke only every 10 ms instead of when new work came.
  All three parts now apply on 1.00 too, in all three games (1.02 unchanged).
- **Motion vectors for animated characters** (#84): the temporal upscalers (DLSS, FSR 3.1/4) got
  the camera's motion only, so moving characters' faces, hair and clothes smeared below Quality.
  Characters now carry their own motion (measured in Uncharted 3 at 960x540 -> 1080p: half the
  error with the camera still, a fifth in a fast turn). GOW3_CHARACTER_MOTION=0 turns it off.
- **The screen wipe covers the whole screen below 720p** (#84): at 540p (the Steam Deck default)
  the black bands of Uncharted 2 and 3's menu/loading wipe left the bottom quarter uncovered.

## 20 October 2026 (0.4.1)

- **Drake's Fortune: crash after about an hour, "out of device memory"** (#63): the game handed
  the port a render target described by garbage registers, a 960x540 image with 6,772 layers
  (16 GB). Creating it took all video memory and the next allocation stopped the game. Render
  targets, depth buffers and textures that would make impossible images (more layers than the
  graphics card allows, or over 1 GB) are now skipped, as garbage textures already were.
- **Uncharted 2: crash in the port's own code** (#56, Radeon 780M): a texture sampler asked for a
  custom border color while the game's border color table was not set (address 0), and the port
  read from there. The color is now read safely; without a table the border is transparent black.

## 19 October 2026 (0.4.0)

- **No more freeze before some cutscenes** (#52): before a cutscene the games remove ~100 small
  blocks of memory, and for each one the port rebuilt the whole surrounding mapping (about 23 ms
  each on Windows, far more under Proton): Drake's Fortune froze 1.6 s, the Steam Deck up to 33 s
  in Uncharted 3's museum. Memory is now mapped in 2 MB pieces, so a removal only touches the
  pieces around it: the same cutscene went from a 1650 ms frame to ~200 ms.
- **Uncharted 3 no longer freezes for good with multithreaded rendering** (#51): a fence write on
  the Vulkan recording thread could fault while the GPU command thread waited for that thread.
  It now never faults. Multithreaded rendering stays off for Uncharted 3 until players confirm
  it (`UNCHARTED_U3_MULTITHREADED_TEST=1` keeps the launcher's choice).
- **Drake's Fortune: enemy grenade launchers no longer blow up on Drake** (#6): from the second
  shot on, the grenade exploded right in front of him. The shot started from a frame-old copy of
  data the GPU writes for the game (a copy shader whose results it reads, the cause of
  Uncharted 2's floating NPCs too); that data now reaches the game before it is used, as in
  Uncharted 2 and 3. No frame-rate cost measured.
- **Drake's Fortune: Infinite ammo keeps the magazine full** (Cheats tab): the game's own switch
  only spared the reserve when reloading; each shot still emptied the magazine.

From kuraeow (Steam Deck), merged:

- **Real upscaling** (#50, asked for in #45): Display -> **Upscaling**: FSR 1, FSR 3.1, FSR 4 or
  DLSS, with **Quality**, Balanced, Performance or Ultra Performance. The game draws below your
  resolution and each frame is upscaled to it before the HUD and menus are drawn, which stay
  sharp. **Native AA / DLAA** (no upscaling) stays as before. Upscaling saves GPU time: it helps
  at 4K or on weaker graphics cards, much less where the processor limits the frame rate.
  Moving characters can smear below Quality (the motion vectors come from the camera only).
  The **Low** preset is now 1080p with FSR 1 Performance (HUD at 1080p). Drake's Fortune above
  1620p draws at 1620p and is upscaled with the HUD at full size.

## 19 October 2026 (0.3.1)

- **Choose the game update in the launcher** (#32): Launcher -> Advanced -> GAME UPDATE.
  **Choose update folder**: your extracted update 1.02, wherever it is and whatever it is called
  (nothing is copied; the launcher remembers it). **Install from .pkg**: the update's `.pkg` file,
  unpacked next to the game folder under the right name. Both check that it is an update for your
  copy of the game (same region) and that nothing the port needs is missing (the update's
  `data.psarc` files, for example).
- **A damaged update is named instead of crashing:** with an update whose game archives are
  incomplete or damaged, all three games stop at start with "files.json -> not found" and
  `ASSERTION: root` (ingame-shaders.cpp). Before a game starts, the launcher now checks the update's
  archives (complete, readable, shader list inside) and says which file is damaged. The port still
  ships no game files: you need your own update PKG. Uses PkgTool from LibOrbisPkg (LGPL-3.0).
  Not fixed yet: the same stop with the US version CUSA02320 (#29, #43, #48), whose update files
  are intact. Until then, renaming its `-patch` folder starts the games as 1.00.
- **Shortcuts on a moved Desktop** (#39): Desktop shortcut and Game shortcuts failed with
  "DirectoryNotFound" when Windows keeps the Desktop elsewhere (OneDrive, a renamed or redirected
  folder). The launcher now asks Windows where it is.

- **"exception 0x20474343" says what failed** (#47): an error nothing caught on the GPU command
  thread ended the game with only that code. The log (and the launcher's crash message) now names
  it, for example "out of memory on the GPU command thread". The crash guard of 0.3.0 no longer
  needs memory to be available: when Windows refuses its snapshot, that readback lands as before.
- **A graphics pipeline the driver refuses no longer stops the game** (#40, AMD Radeon RX 9070 XT):
  "Failed to create graphics pipeline: ErrorInitializationFailed" ended every game at start. The
  pipeline is now created again without the pipeline cache; refused again, only the draws that need
  it are skipped and the log names it ("GPU: the driver refused graphics pipeline ...").

From SuperMedo/uncharted_pc (thanks to kj2223 and hugouchoasborges), ported:

- **Sun shadows no longer flicker with TAA, FSR or DLSS/DLAA** (#261, kj2223). The sun shadow pass
  (the same shader in all three games) rebuilt each pixel's position from the scene depth without
  the anti-aliasing's sub-pixel offset, so its shadows moved against the geometry every frame. It
  now gets the same offset as the scene.
- **Steadier anti-aliasing** (#261, kj2223): Uncharted draws twice into the picture it shows, and
  the port started a new frame on both. The second start threw away the anti-aliasing's history
  every frame and skipped every other sub-pixel position. It is now one frame start per frame
  (measured: 600 starts per 10 s at 60 FPS, was per 5 s).
- **Steam Deck and other Linux AMD/Intel GPUs** (#94, hugouchoasborges): memory binding goes to a
  GPU queue that supports it (on Steam Deck the graphics queue does not), and large texture
  conversions are split to stay within the GPU's limit (65,535 work groups on some GPUs). On
  NVIDIA and AMD on Windows nothing changes.

From kuraeow (Steam Deck), merged:

- **16:10 aspect ratio** (#35): 720p becomes 1280x800 (the Steam Deck's screen); the games show
  more at the top and bottom. Resolution buttons are labelled for the chosen ratio, with the exact
  render size below.
- **Cutscene movies keep their shape** at sizes that aren't 16:9 (#36): black bars instead of a
  stretched picture, at 16:10 and ultrawide.
- **Update 1.02 merged into the game folder** (#37) is recognised even when its param.sfo still
  says 1.00 (no more "install update 1.02" warning, cheats work).
- **Steam Deck / Proton**: one set of launcher options for every Steam shortcut (#38).

## 19 October 2026 (0.3.0)

- **Random crashes after a while, "exit code 3"** (#30, #24, #27): GPU results reach the game's
  memory a little late, up to a frame or two after the game was told the GPU had finished. When
  the game reused that memory in between (an area unloading, a cutscene starting), the late copy
  overwrote the game's new data: crashes in the game's memory allocator, a function pointer turned
  to 0, and similar. Late GPU data now never overwrites a 64-byte block the game changed in the
  meantime. In 51 minutes of Uncharted 3 it stepped in 38 times, once exactly where the game had
  crashed before. Logged as "Readback guard: kept ..."; `GOW3_READBACK_GUARD=0` turns it off.
- **Uncharted 3: blurry view when aiming** (#16): the aiming depth of field read its focus
  distance from a one-pixel image the GPU renders, which was only copied back to the game in
  Uncharted 2. It stayed at 0 m and everything behind Nate was blurred. Uncharted 3 now gets it
  too; the focus follows what you aim at.
- **Fewer shader hitches**: compute shaders found in the game's command buffers are compiled in
  the background before the GPU needs them, instead of all at once on the frame that first uses
  them (`GOW3_COMPUTE_PRECOMPILE=0` turns it off).
- **Cheats** (#26): the launcher's new **Cheats** tab: Invincible, Infinite ammo, No reload
  (Uncharted 2 and 3), Infinite grenades and One-hit kills (Uncharted 3). They switch on the games'
  own developer options; they need update 1.02. In Uncharted 1 and 2 the infinite ammo option lives
  in the games' settings system, which resets it every frame, so the places that read it are
  changed instead.
- **Ultrawide shows more at the sides** (#20): at 21:9 and 32:9 the three games kept the 16:9
  picture's width of view and cut off the top and bottom instead. They now keep the 16:9 view top
  to bottom and widen it to the sides, culling included (nothing missing at the screen edges);
  checked in all three games at 3440x1440 and in Uncharted 2 at 5120x1440. 16:9 is unchanged.

- **Start a game directly** (#25): Launcher -> Advanced -> **Game shortcuts** puts one desktop
  shortcut per game; each starts its game with the launcher's settings, without the launcher
  window. From a command prompt: `"Uncharted Launcher.exe" --play u1` (or `u2`, `u3`). A
  double-click on a game's card in the launcher plays it too.
- **Launcher: buttons on the Advanced page were hidden** (log folder, save folder, shortcuts,
  shader cache, reset, check for updates): the page had grown taller than the window. Pages
  that don't fit now scroll, and they are grouped as Shortcuts, Folders and Troubleshooting.
- **Launcher: easier to use.** More room for the settings (a smaller header; the window opens
  taller when the screen allows), plainer descriptions, recommended choices marked with a star,
  and Quick Setup marks the preset that suits your graphics card ("Recommended").
- **Warning without update 1.02** (#24): most of the port's game fixes, crash fixes among them,
  are written for update 1.02 and are skipped on 1.00 (the log listed them as "NOT applied"
  without saying why). The launcher now says so once when you press Play, and the log names the
  game version and where the update goes.
- **Patreon** link in the launcher header.
- **Crash messages say what crashed.** "Exit code 3" is the port's general crash code; the launcher
  now shows the crash's details from the log (what failed and where) and asks for boot.log, instead
  of only the exit code (#24, #27).
- **Freeze reports.** When a game shows no new frame for 30 seconds, the start script records where
  every thread is stuck (hang-<time>.txt in the log folder, a few KB) and the launcher points to it.
  Please attach it with boot.log to a GitHub issue: every freeze can now be traced to its cause.
  A fault inside the port's own fault handling now stops with a report instead of freezing.
- **Freeze in any game** (since 0.2.0, seen in Uncharted 3): when a shader's resource table held a
  garbage pointer, the 0.2.0 fix for the rooftop freeze looked that address up in its page table
  without a range check, which faulted inside the fault handler, endlessly. It now only looks up
  game addresses; such loads are skipped as before 0.2.0.
- **Crash after a long session** (seen in Uncharted 2 after 38 minutes, `Uncharted.exe+0x11474b`):
  when the texture cache's image table grew while a depth buffer got its stencil part, the code
  went on using the old location of the depth image. Fixed there and in one other place with the
  same mistake.

From the work on a ROG Zephyrus G14 laptop (Radeon 780M and RTX 5060 Laptop GPU), merged:

- **Graphics cards with 6-8 GB: no more texture thrashing.** The texture cache started throwing
  textures out at about 40% of the card's memory, so on 6-8 GB cards it evicted hundreds of
  textures every few seconds and reloaded them (75-150 ms hitches about every 15 s on an 8 GB RTX
  5060). It now goes by the driver's memory budget, as on integrated GPUs. The crashes in #24 (GTX
  1660 SUPER, 6 GB) all came while it was evicting.
- **Uncharted 3 could stop responding when a level started** (a guest write looping on a page a
  GPU readback still protected): the readback is now landed at once. A second guard beside the
  rooftop fix of 0.2.0.
- **Update 1.02 from exFAT and FAT32 drives** (USB sticks): those drives hold no links, so the
  update was skipped and the game ran as 1.00. The runtime now lays the update over the game itself.
- **"Error 1450" at start on PCs with 16 GB of RAM**: the game's memory is now committed as it is
  used instead of all at once.
- **Memory corruption after partly freeing game memory** (flexible memory split): could zero live
  game data on any PC, a candidate for random crashes.
- **Multithreaded rendering**: readbacks could be landed twice (heap corruption) in Uncharted 3;
  fixed. Uncharted 3 ran 24% faster with it on the 780M.
- **Drake's Fortune compiles shaders in the background** again (it froze 1-2.4 s in each new area;
  now about 120 ms at worst). UNCHARTED_U1_SYNC_SHADERS=1 restores the old way.
- **RTX laptops: fewer hitches** (render targets no longer get a driver allocation each), **AMD
  GPUs**: fewer allocation stalls at level start and a cheaper second-queue fetch.

## 18 October 2026 (0.2.3)

- **Uncharted 1: guns flickering flat grey** (#23). With multithreaded rendering on, Nate's rifle
  and pistol (and at times other objects) were drawn flat grey on some frames. Drake's Fortune now
  always runs without multithreaded rendering: it gained nothing from it there.
- **Uncharted 2 crash `m_lookAt.X() == m_lookAt.X()`** (#24): the point Nate looks at could become
  invalid (NaN) and the game stopped at its own check, as Uncharted 3 did in #13. Uncharted 2 now
  resets that point to where the player looks instead, as the Uncharted 3 fix does.
- **Frame captures** (diagnostics): a capture requested during play could end the game when a
  shader had a buffer at address 0; such buffers are now skipped.

## 18 October 2026 (0.2.2)

- **Windows 10: all three games crashed at start on some PCs** (#22): the games' job code runs on
  PS4 fibers with their own stacks, and some Windows 10 builds end the process when an exception
  resumes on a stack the thread doesn't have registered. The port now registers the stack in use
  before resuming.
- **Uncharted 3 crash in Syria** (#21, `PatchImageSharp`): a damaged image description while a
  level loads made the shader translator stop the game; that one write is now skipped.
- **All the Collection's languages.** Launcher -> Advanced -> **Language**: the language the PS4
  would be set to, from the 22 the games have text for (English US/UK, French, German, Italian,
  Spanish and Latin American Spanish, Portuguese and Brazilian Portuguese, Dutch, Danish,
  Norwegian, Swedish, Finnish, Polish, Czech, Greek, Russian, Turkish, Japanese, Korean,
  Traditional Chinese). Each game's main menu then offers it next to English: press **Square**
  to switch its menus and subtitles. Spoken language depends on your copy of the game: the list
  marks the languages it has voices for (the European CUSA02344 has English, Polish, Russian and
  Turkish). Before, the port always reported English, so only English was offered.
- **Unlock all chapters.** Launcher -> Advanced -> **Unlock all chapters**: every chapter in each
  game's Chapters menu (Drake's Fortune 1-22, Among Thieves 1-26, Drake's Deception 1-22) and the
  Brutal difficulty from the start, without finishing the games first. It turns on the
  Collection's own "unlock by default" switches, so saves and progress are untouched. Off by
  default; needs update 1.02.
- **Launcher errors are shown and saved** (`analysis\uncharted-user\launcher-error.log`); an error
  at start used to close it silently.
- **Long sessions:** a diagnostics list that grew with every GPU image and buffer made in a session
  (never shrunk, searched on every free) is now kept only with the GPU fault diagnostics on.

## 18 October 2026 (0.2.1)

- **NVIDIA DLAA and AMD FSR 4 Native AA are in the package.** The 0.2.0 build could not fetch
  their files (tools/fetch_aa_files.sh copied the Vulkan headers from `//clang64` on the build
  runner, where the MSYS2 root is `/`), so the menus offered only TAA and FSR 3.1.

## 18 October 2026 (0.2.0)

- **Smooth frame pacing above 60 FPS with VSync (all three games).** With a frame rate limit at or
  above the monitor's refresh rate and VSync on, the game made frames faster than the monitor
  shows them; they queued up behind VSync and the game was held back in bursts: two or three
  quick frames, then a long one. Now the game follows the monitor (its refresh rate, up to
  120 Hz), as it follows a 60 Hz screen: on a 75 Hz monitor at the 120 setting, Uncharted 3's
  rooftops and Uncharted 2 run at an even 75 FPS, 1% lows 32 -> 66 FPS (Uncharted 2: 22 -> 67),
  no frame over 20 ms (28% before). It switches live with the in-game VSync and limit; monitors
  above 120 Hz, VSync off and lower limits keep the earlier pacing. New frame rate limits 75, 90
  and 100 (launcher and in-game menu) for those monitors and variable refresh rate screens.
- **Uncharted 3: no freeze on the Cartagena rooftops (chapter 2).** The game froze about 30
  seconds into the rooftop chase at 120 FPS: it cleared a field of its next ray-cast batch in
  memory still guarded for a GPU result on its way back; the port's fault handler let the write
  fault again and again, and the result it waited for was never collected because the GPU
  thread was idle waiting for the game. Such a write now collects the result at once (or waits
  for the GPU when it is still running). Shorter versions of the same loop, about 30 a second
  in that area, were small hitches in all three games. While the GPU thread waits for the GPU
  at the end of a frame it now also collects finished results (fewer long frames at high
  frame rates).
- **Cutscenes play at normal speed with the frame rate above 60.** The Collection's cutscenes are
  prerecorded movies that show one picture per screen refresh; with the frame rate unlocked
  (120) they ran fast and out of sync with their sound (reported in Uncharted 3). While a movie
  plays the game now gets 60 Hz refreshes; gameplay keeps the chosen frame rate. (The same fix
  as in SuperMedo's Uncharted port.)
- **Anti-aliasing: TAA, AMD FSR 3.1 and FSR 4 Native AA, NVIDIA DLAA (all three games).** In-game
  menu -> Image -> Anti-aliasing, or launcher -> Display -> Anti-aliasing: Off (the game's own, as
  before), TAA, FSR 3.1, FSR 4 (listed when the GPU can run its 8-bit model; tested on an RTX 5080)
  or DLAA (NVIDIA RTX), with sharpening for FSR and DLAA. It works at any render resolution, ultrawide
  included, and switches at once from the menu. Jagged and shimmering edges (roof tiles, railings,
  foliage) are much calmer: edge flicker about halved in Uncharted 2, a third of what it was in
  Uncharted 3 (frame-to-frame measurements). The camera's motion comes from the games' own
  motion-blur velocity pass, an idea from SuperMedo's Uncharted port. Turning the camera leaves no
  trails; TAA softens the picture slightly. DLAA calms edges the most (Uncharted 2: a third of the
  flicker). Cost on an RTX 5080 at 4K in Uncharted 2: TAA and FSR 3.1 kept the 120 FPS cap, DLAA
  ran at about 84 FPS (NVIDIA's model, about 3.6 ms a frame at 4K) and kept 120 FPS at 1440p.
  The package carries NVIDIA's `nvngx_dlss.dll` (DLSS SDK 310.9.16) and the FSR 4 native model
  (tools/fetch_aa_files.sh fetches them for builds). UNCHARTED_TEMPORAL_AA=0 turns it all off.
- **Ultrawide monitors (GitHub #7).** Launcher -> Display -> Aspect ratio: 16:9, 21:9 or 32:9. The
  render resolution's height is kept and the picture widened (1080p at 21:9 is 2560x1080, 1440p
  is 3440x1440); all three games show more to the sides with nothing stretched, the HUD and menus
  keep their layout, aiming stays centred. Use Borderless or Fullscreen on the ultrawide monitor.
  Drake's Fortune, which runs out of GPU memory above about 1620p, steps down in the same shape.
- **Draw distance options (Uncharted 2 and 3, experimental).** In-game menu -> Image -> Draw
  distance: object fade distance (up to 4x the game's own, which fades objects and details in at
  0.58 of their authored distance), sun shadow distance and lamp/torch shadow distance (up to 3x
  the game's 40 m). Applied live; higher values cost some FPS.
- **Fewer hitches after loading (shader compiles).** On NVIDIA GPUs the cached shaders were only
  prepared when a level first needed them, and compute shaders were translated in the middle of
  the frame: Uncharted 3 hitched for 90-170 ms several times right after a load. The shader cache
  now loads in the background while the title screen shows (2,400+ pipelines in 7-16 s), so none
  are compiled in play. GOW3_ON_DEMAND_PIPELINES=1 keeps the old start.
- **In-game menu: your own shortcut (GitHub #15, #19).** Controls -> Menu shortcut: LT + right
  stick click (default), View + Menu together (no clash with the sniper zoom), none, or Custom:
  select "Custom menu shortcut" and hold any two controller buttons (or one the game does not
  use, such as Guide or a paddle). "Menu key (keyboard)" changes Insert to any key. The menu's
  title shows the current shortcuts.
- **Uncharted 3: no crash climbing the fort in chapter 8 (GitHub #13).** When the point Nate looks
  at became invalid (NaN), the game stopped (`m_lookAt.X() == m_lookAt.X()` in UpdateFocus); now
  it resets that point the way the game already does when it is far off.
- **Fewer crashes in all three games (GitHub #11, #12, #17).** The most reported crash, the game's
  check that the GPU finished frame N-3 (`m_gfxEopTick` in FrameBegin), came after short stalls on
  slower or busier PCs: the game's GPU event thread had not marked that frame done yet. The game
  now waits for it (a brief hitch at most) instead of stopping. UNCHARTED_FRAME_WAIT=0 keeps the
  original check.
- **The aim sensitivity sliders go higher in all three games.** Options -> Controls -> Aiming ->
  Sensitivity now reaches five times its old maximum in Uncharted 1 and 2 (a notch is half the old
  maximum, the second notch the old right end) and four times in Uncharted 3 (the left half keeps
  its fine steps, the right half goes up to 4x), for fast aiming with the mouse or a controller.
  (The games' own range checks are lifted; UNCHARTED_AIM_RANGE=0 keeps the original sliders.)
- **Controls in the in-game menu.** Insert (or LT + right stick click) -> Controls: rebind every
  action for keyboard, mouse and controller (select a slot, press the new key or button), mouse on
  and off, mouse sensitivity, invert, button icons, a controller camera speed (Uncharted 2 and 3,
  not yet in every camera) and Reset. Changes apply at once and are kept in the game's settings
  file, which the launcher's Controls tab now reads and writes too.
- **Keyboard and mouse button icons in all three games.** While you play with the keyboard and
  mouse, every button icon the games draw in their text shows your keys and mouse buttons instead
  of the PlayStation buttons: tutorial hints ("Press SPACE to jump"), menu legends (SPACE Select,
  CTRL Back, ESC Resume), the sticks (WASD, the mouse) and the d-pad (1-4). The icons are drawn
  from your own bindings, so remapped keys show up too. The prompts the games draw in the world
  (the Triangle beside a chest, the round Square/Circle/Cross buttons, Uncharted 3's L1/R1/L2/R2)
  change too: the same button picture with your key in place of the PlayStation symbol, or a mouse
  for the mouse buttons. Touch a controller and its icons come back at once; use the keyboard
  again and they switch back. Xbox and other controllers get their own buttons (A B X Y in their
  colours, LB RB LT RT, Menu, View); PlayStation controllers keep the game's own icons. Launcher:
  Controls -> Button icons: Auto (the device you use) / Keyboard and mouse / PlayStation / Xbox.
- **Launcher: controller bindings with button icons.** The Controls tab shows each action's
  controller button as an Xbox or PlayStation icon (Auto: the controller you have connected).
  Click a controller slot, then press a button on the controller to rebind it; right-click clears
  it; a button already in use swaps with the old one. The triggers and sticks stay Aim, Fire, Move
  and Look. The in-game icons follow your bindings. The launcher window can now be resized.
- **Uncharted 2 and 3: raw mouse look.** The mouse turns the camera directly (1:1, no stick
  emulation, no acceleration or dead zone), at the same speed horizontally and vertically;
  Sensitivity and Invert Y in the launcher apply. Uncharted 1 still uses the mouse as a stick.
- **Low-end PCs: the games no longer take every core of a 4-6 core CPU.** The engine's six job
  threads spin while waiting for work (on the PS4 each has its own core). On 4 cores they used 390%
  of 400% CPU and starved the GPU thread: Uncharted 3's title screen ran at 19-27 FPS on an RTX
  5080, its first level at 5-7 FPS on an integrated GPU. The engine has its own sleep mode for idle
  job threads (a job wakes them); it is now always on (launcher: Performance -> Idle game threads:
  Sleep / Spin). 4 cores: 60 FPS at the title and in Uncharted 1 and 3, about 1.5 cores used;
  integrated GPU: 27 FPS at 720p (was 5-7). 16-core desktop: Uncharted 3 uses about 2.3 cores
  instead of 6.4, and Uncharted 2's heaviest view runs 76.5 -> 78 FPS (uncapped).
- **540p render resolution** (launcher, Display) for integrated GPUs, with FSR 1 to the screen:
  tested in all three games; Uncharted 3 on an AMD integrated GPU 27.5 -> 33 FPS against 720p.
- **Less GPU command thread work per draw:** the resource-table checks before every draw asked
  the memory map for every table (6% of that thread); confirmed mapped areas are now remembered
  until memory is unmapped (the checks went from 12% to 6.5% of the thread in Uncharted 3).
- **Fewer crashes from half-written resource tables:** a draw whose tables held garbage while the
  game was still filling them made the shader compiler stop the game ("Thread ID buffer addressing
  is not supported outside of compute"), and one bad pointer turned that table read into zero for
  the rest of the session. Such a draw is now drawn once with the bad entries empty.
- **Uncharted 3: Sully's shirt flashing as big flat-coloured triangles fixed** (at 40-60 FPS; it
  never happened at 30). The cloth simulation shaders sum values in shared GPU memory the way the
  PS4's 64-lane waves allow: without waiting between steps inside one wave. NVIDIA runs each wave
  as two 32-lane halves, which raced, so the shirt exploded on every other frame whenever the game
  used 2 cloth sub-steps (40-60 FPS; at 30 FPS it uses 3). Both cloth solvers (the shirt uses two)
  now get the waits, including inside the loop that repeats until the cloth settles (a value read
  at the end of one pass was overwritten at the start of the next). On an AMD GPU it never happened.
  Only those two shaders recompile, the rest of the shader cache stays.
- **Uncharted 3: Nate's legs flickering and NPCs floating a little fixed.** Like Uncharted 2 in
  0.1.2: the results of the game's small GPU copies now come back with the frame's "done" fence.
- **Much faster at 4K and in crowds (all three games), measured on a Ryzen 9 9950X3D + RTX 5080,
  frame limit 120.** The heavy scenes are limited by the CPU thread that feeds the GPU, not by the
  GPU, so this helps every PC (render resolution makes no difference there):
  - Uncharted 3 re-copied every crowd character's mesh data (about 43 MB per frame) from game memory
    on every use, because each piece is small enough to be treated like per-frame constants. Data
    that stays the same is now kept on the GPU and only copied again if the game changes it.
  - Fence values the GPU writes for compute work landed on memory pages that also hold GPU data, and
    each such write waited for the GPU (about 1.7 ms, ~40 times a second): now written directly.
  - Fewer barrier checks (overlapping views of one buffer counted once), lock-free checks before
    each draw, fewer thread wakeups for GPU interrupts, faster page fetches on a second GPU queue.
  - Results at 4K: Uncharted 3's Cartagena market 64 -> 79 FPS at the checkpoint and 69 -> 90 in
    the crowd (the items above; the second-queue fetches before them took a crowded street 46 -> 58),
    Uncharted 2's heaviest view 38 -> 99 FPS, Drake's Fortune 95 -> 101 FPS.
- **Multithreaded rendering is now much faster** (launcher, per game):
  compute fences no longer stop the recording thread. 4K: Uncharted 3 100 FPS, Uncharted 2 118,
  Drake's Fortune 117 (the 120 limit). On 4 CPU cores Uncharted 3 at 1080p runs at the 120 limit
  (90 without). 5 minutes of Uncharted 3 with it: no errors.

### Known issues (0.2.0)

- **Drake's Fortune:** the grenade launcher can kill the player (GitHub #6). Being looked at.
- Uncharted 3 on an AMD integrated GPU stopped shortly after a level started in earlier builds (the
  game waited for an internal job): not seen any more in a 6-minute test with the rooftop freeze
  fix (probably the same cause). Please report it if it still happens.

## 16 October 2026 (0.1.2)

- **Uncharted 2: blurry background while aiming fixed.** The aiming depth of field reads its focus
  distance from a 1x1 texture the GPU renders; it was never copied back to the game (the distance
  stayed 0, so everything behind Nate was out of focus). Tiny GPU-rendered textures now come back
  with the frame's "done" fence (Uncharted 2 only; the other two games do not need it). Above 1080p
  the game also measured the depth at the wrong spot (the crosshair's 1080p HUD position over the
  larger render): that position is now scaled to the render size, so it is right at every
  resolution.
- **Uncharted 2: GPU hang ("Device lost") after a long session fixed.** The treasure pickup effect
  takes some buffer descriptors from table slots that hold other data; one that pointed into mapped
  memory with ~1e9 records hung the GPU. Such descriptors (unaligned, huge stride or over 64 MiB)
  are now ignored for its two shaders.
- **Multithreaded rendering:** the Uncharted 3 crash within minutes (flips overtook the frame's
  fences; the GPU-written range set was read and changed by two threads) and the Uncharted 2
  floating NPCs (fences did not wait for their readbacks on the recording thread) are fixed;
  Uncharted 3 ran 8 minutes crash-free. Still experimental.
- **Faster heavy scenes:** optimizations that were off since the grey-world and floating hunts
  (threaded recording, draw preparation, prepared resources, deferred stream copies, parallel
  copies) are on again by default: heaviest Uncharted 2 view 43.8 -> 49.0 FPS, Uncharted 3 about 9%
  less GPU-command-thread work.
- **74-85% less GPU readback in all three games:** windows the CPU stopped writing into cool down
  after 3 s, and small GPU writes come back on their own instead of with the whole 64 KiB around
  them (the effect shaders' data the game never reads made up nearly all of it). Uncharted 2
  2.9 -> 0.46 GB per 5 s, Uncharted 3 3.1 -> 0.48, Drake's Fortune 0.63 -> 0.16.
- **Frame rate limits 40, 45, 50 and 55** in the in-game menu and the launcher.
- Profiler (`GOW3_PROFILE`): no longer freezes or crashes the game (own lock-free stack unwinder).
- New diagnostics: `GOW3_READBACK_STATS=1` (readback bytes per window, why and who wrote them),
  `GOW3_TRACE_PS=<hashes>` (render targets, viewport, textures and constants of a shader's draws).

- **Uncharted 3: crash in the Cartagena museum and characters flying around fixed.** GPU results
  were copied back to the game without protection: a late copy could land on data the game had
  just written (garbage pointers, a broken GPU command list). New GPU data is now read-protected
  until it is back; the game's reads of it wait, everything else runs on.
- **Uncharted 2: NPCs coming from the sky fixed.** The game places characters on the ground with
  GPU ray casts and reads the results once a GPU "done" counter is set. That counter was written
  when the command was recorded, before the GPU had run it, and a shortcut from 15 October threw
  nearly every result away. The counter is now written once the results are back in game memory,
  in strict order with the frame's other counters (as on the console).
- **Uncharted 2: NPCs floating in the air, Nate's feet sinking into the floor and NPC legs
  flickering while they walk fixed** (also in 0.1.1). The game reads back data that a GPU copy
  shader writes every frame; the port returned it with the next submission, a frame late. Those
  small copies now come back before the frame's "done" fence the game waits for (no FPS cost; the
  shader's large copies, which the game never reads, are left out). Every preset queues 1 frame
  again; the launcher moves preset settings back from 2 to 1.
- **Faster heavy scenes:** guest-memory copies for the GPU no longer take a lock (10-15% of the GPU
  command thread in heavy Uncharted 2 scenes: 7.9 -> 6.4 microseconds per draw); GPU checkpoints
  (crash diagnosis, ~2%) only on request (`UNCHARTED_GPU_CHECKPOINTS=1`).
- Protected copy-back without the cost: pages where the game reads its own data beside GPU results
  and results the game reads every frame (such as scene brightness) stay readable; Uncharted 2
  went from 33-46 to 60 FPS with protection on. A deadlock that hung Uncharted 3 while loading
  with protection on is fixed (uploads copy through an unprotected view of the same memory).
- Tracking of small GPU writes no longer grows over a session.


- **First release, 0.1.0 (beta).** The package now carries `LICENSE.txt`, `CHANGELOG.md` and
  the licenses of all bundled libraries (`licenses/`).
- **Faster heavy scenes with every preset: 2 frames queued** (Low and Medium had 1). With 1 the
  CPU waited for the GPU to finish the previous frame every frame; in Uncharted 2's museum
  (1080p) that was 44-47 FPS, with 2 it is 56-59. The launcher moves existing settings over
  (not for a game with multithreaded rendering, which Uncharted 2 needs with 1).
- The game and launcher windows name the port's author in their title bar.
- **Launcher updates:** at start the launcher looks for a newer release and offers
  **UPDATE TO x.y.z**; it downloads the package, checks its SHA-256 against the one GitHub publishes,
  installs it with the new version's own installer (`tools/apply_update.py`: saves, settings and the
  shader cache stay; on any error every replaced file is put back) and restarts. Advanced tab: the
  check can be turned off or run now. Tested against a local fake release (install, restart,
  cleanup) and a locked file (full rollback of 1,077 files).
- **Automatic releases:** `tools/release.sh X.Y.Z` pushes the tag `vX.Y.Z`; GitHub Actions builds
  the package and publishes the release (marked Latest) with the notes from `tools/release-notes/vX.Y.Z.md`.
- Profiler: call stacks (`profile_*_stacks.txt`), also of the game's own thread that submits the
  frames, so a wait shows who waits.
- Packaging: the license step no longer hangs on GitHub Actions (the MSYS2 root `/` made
  `//usr/bin/pacman` a network path).
- **Uncharted 2: Nate floating above the ground or invisible fixed.** A CPU shortcut for one of
  the game's small GPU copy shaders (6a6422a9) copied the player's data before the game had
  finished it; that copy runs on the GPU again.
- **Uncharted 2: crash with multithreaded rendering fixed** (game assert `m_gfxEopTick` in
  `FrameBegin` after 1-3 minutes): GPU memory copies and fills no longer overtake the frame's
  end-of-pipe timestamp. Still open with 2 frames queued, see the known issues.
- **Heavy scenes: stalls removed.** CPU writes into ring memory whose old GPU data was still being
  read back waited for the GPU (a third of each frame with multithreaded rendering); the
  per-draw stream buffer grew from 128 to 512 MiB (it filled up with 2 frames queued);
  "Frames queued 2" is now the High preset's default. Heavy Uncharted 2 scenes: about 33 -> 40-45
  FPS (4K, multithreaded off).
- Built-in sampling profiler for the GPU threads (`GOW3_PROFILE=<seconds>`).
- **Drake's Fortune: bullet impacts, bullet holes and blood.** The game traces its bullet and
  decal rays on the GPU (`Cs_RayTrace`, `Cs_RayTraceGatherHits`) and reads the hits once the
  frame's end-of-pipe event fires. That event was signalled before the GPU had produced the
  hits, so every shot read as a miss. The results are now copied back asynchronously with their
  pages read-protected until they arrive.
- **Drake's Fortune: cutscene movie colours at 1620p** (grey picture with diagonal coloured
  lines). The GPU reads the movie's colour plane with rows padded to 64 texels (1472 at 1620p);
  the scaled frame copy now writes its rows the same way.
- **Frame-rate limit capped at 120.** At 140-160 FPS Drake's Fortune's movement broke (Nate fell
  through the world). The launcher and the in-game menu offer 30 / 60 / 120; older, faster
  settings are read as 120.
- **Launcher and in-game menu settings stay in step.** Closing the in-game menu no longer
  overwrites a change made in the launcher while the game runs, and the launcher shows what the
  in-game menu saved.
- **Uncharted 2: crashes at treasure pickups and in the Treasures screen fixed.** CPU-rewritten
  per-frame data (skinning tables) was corrupted by a byte-compare merge of GPU data; the merge
  is gone, and resource descriptors that cannot be real are ignored instead of crashing the GPU.
- **Uncharted 2: frame rate no longer falls over time.** In a busy scene it slid from about 42 to
  31 FPS within minutes: a per-page list of GPU writes grew every frame and was scanned on every
  buffer binding. Found with a new built-in sampling profiler (`GOW3_PROFILE`); the frame rate now
  stays level.
- **Frame drops in heavy scenes reduced.** CPU writes next to GPU data in flight no longer wait
  for the GPU; scalar copy shaders run on the CPU; GPU readbacks are asynchronous.
- **Uncharted 2: torch follows Nate** (launcher option, Advanced tab). *In test.*
- **Update 1.02** used automatically when it sits next to the game folder.
- **Render resolution above 1080p:** Uncharted 2 and 3 up to 4K, Drake's Fortune up to 1620p with
  FSR 1 (its memory layout); cutscene movies scaled to the render size.
- Keyboard and mouse: mouse look and remappable controls per game; display modes fixed.

## 14 October 2026

- **Grey/black ground and unlit Nate fixed:** constants the GPU writes are synced back to the CPU.
- **Sound effects** (gunfire, footsteps, ambience) fixed.
- **Dark interiors:** fire and flashlight light fixed; bright hair and cobwebs fixed (MUBUF ADDR64
  loads).
- Uncharted 2 and 3 start from the Collection menu; the Uncharted 2 new-game freeze fixed.
- The Uncharted launcher: per-game settings, quality presets, render resolution.

## 13 October 2026

- Gameplay reached: the Collection's game code runs natively on Windows.
