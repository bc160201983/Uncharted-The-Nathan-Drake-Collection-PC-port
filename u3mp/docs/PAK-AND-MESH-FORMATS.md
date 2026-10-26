# PS3 -> PS4 pak conversion findings (U3, pak version 0x0a79)

Source: pak-conversion research agent report (2026-10-26). Nothing here has been tested in-game yet.

## Key correction
- PS4 UI icons are looked up in a **material table** keyed by `sid(material name before ',')`, not by texture hash.
- `0x11b7600` sets UI+0x28 = level("net")->+0x150 once that level is in state 8, and UI+0x30 = level("menu")->+0x150. The icon code (0x7521a0, and the inlined f_97202c) scans UI+0x28.
- level+0x150 is built only from a GEOMETRY_1 item's materials bound against an EFFECT_TABLE. PS3 MATERIAL_TABLE_1 is ignored by the PS4 loader.
- So a PS4 net.pak needs EFFECT_TABLE + TEXTURE data + GEOMETRY_1 (materials), with login order EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1.
- Icon ids are sids of PS3 material names, e.g. 0x7e2c2757 = sid("general/net/mode-buttons/customgame").

## Container
- Header (u32, file endianness): 0x00 u16 magic 0x0a79 + u16 flags; 0x04 hdrSize; 0x08 loginPage; 0x0c loginOffset; 0x10 pageCt; 0x14 pageEntryOff=0x2c; 0x18 numFixupTables=8; 0x1c fixupDirOff; 0x20 vramSize; 0x24/0x28 VRAM desc table page/offset; 0x2c PageEntry[] {fileOff,size,x}.
- Fixup entry {u16 srcPage, u16 dstPage, u32 off}: `*(page[src]+off) += base(page[dst])`.
- Page header: PS3 0x10 bytes `{u32 link,u32 size,u32 flags,u16 ?,u16 n}`; PS4 0x14 bytes `{u64 link,u32 pkgIdx,u32 size,u16 ?,u16 n}`; then n x 16-byte entries `{u64 name,u32 itemOff,u32 0x12345}`.
- Item header 0x20: `{u64 name,u64 type,u32 size,0x12345,u32 nameSid,u32 typeSid}`; the loader (0xdd4620) rebases the name/type pointers itself (not in the fixups).
- sid = CRC-32, poly 0x04C11DB7, MSB first, init 0, no final xor.
- PS4 page 1 holds only the login table (at 0x30) and the VRAM desc table, both unlisted; there is no VRAM_DESC_TABLE item type on PS4.

## Texture descriptor
| PS3 | field | PS4 | field |
|---|---|---|---|
| 0x04 | vramOffset u32 | 0x08 | vramOffset u64 |
| 0x08 | vramSize | 0x10 | vramSize |
| 0x0c | hash | 0x14 | hash |
| 0x14 | GCM format (06 DXT1, 08 DXT45, 05 ARGB8) | 0x1c | DXGI (0x47 BC1, 0x48 BC1 sRGB, 0x4d BC3, 0x50 BC4, 0x53 BC5) |
| 0x1c | mips | 0x24 | mips |
| 0x20/0x24 | w/h | 0x28/0x2c | w/h |
| 0x30 | path ptr | 0x38 | path ptr |
- PS4 +0x20 bytes {0,0,0x0f if sRGB,1}; +0x34 streamFlags (2 world, 3 UI).
- hash = sid(short path), same scheme on both.
- Texels: PS3 linear DXT, mips packed. PS4 per mip padded to 8x8 blocks, 8x8-block tiles row-major, Morton order inside a tile. Pure block shuffle, no re-encode.

## GEOMETRY_1 v0x10 (PS4)
- +0 version 0x10, flags (bit0 foreground); textures +0x38/+0x40 (0x28 each); materials +0x48/+0x50 (0x48 each: name, shader, params, effectHashHex, 0, bindings, nParams, nBind).
- Binding 0x28: {param ("g_tColorMap"), shortName, fullPath, i32 -1, u32 runtime, 0}.
- Background-level path: 0x10f6700 -> CreateGeometry 0xf48060 -> job 0xf52e70; foreground levels use 0xf534d0 (a mismatch traps).
- EFFECT_TABLE v7 {n, 7, entries[0x18] = {EFFECT*, fxIdx, u64 hash}}.

## Converter
- `convert_net.py ps3/net.pak out/net.pak --ref ps4/menu.pak --ingame12 ps4/menu-ingame12.pak out/net-ingame12.pak`
- `ps4check.py out/net.pak --want 7e2c2757 23e9be55 b675afdb [--png DIR]`
- Options: `--effect logo|edge`, `--foreground`, `--linear-color`, `--stream-flags N`.
- Output: 423 textures, 296 materials, effects copied from PS4 menu.pak (menu-logo effect `hud/textured-2d.fx`).

## Open
1. Runtime test (zero-mesh geometry, reused effect, TEXTURE_TABLE consumer unknown).
2. "net" level entry in the PS4 level list (level-def ReadTxtFile 0x1294bb0, packages via RequestPackage 0xdd6d10, path `%spak%i/%s.pak`); must load as a background level.
3. 171 materials used a PS3 shader with a different vertex program; small tint differences possible.
4. sRGB vs linear colour maps (`--linear-color` if wrong).
5. MP HUD icons (e.g. 0xc1df5e2e) need a merged PS4 hud.pak.

---
## 6. Merged PS4 hud.pak (multiplayer HUD materials) — `merge_hud.py`, output `out/hud.pak`

### Finding: the HUD materials are not missing, only their names are
* Retail PS4 hud.pak: 2 pages, 205 materials (all named `dir/file:variant,<guid>`), 205 EFFECTs, 347 textures. It still passes `ps4check`.
* **All 185 PS3 hud.pak materials have a PS4 equivalent.** Replacing the last `/` with `:` in the PS3 name gives the retail PS4 name, and no PS3 material has more textures than its PS4 version.
* The multiplayer code still searches the hud table (UI+0x20) for the **PS3 slash-form sids**. Example, PS4 0x62ea60:
  * the linear search at 0x62eb50 compares against `0xc1df5e2e = sid("general/hud/store/purchased")`
  * retail hud.pak only has `general/hud/store:purchased` = 0x658daf73
  * so the search returns -1 and the icon is blank.
* Slash-form hud/menu sids used in PS4 code:

  | sid | material | used at |
  |---|---|---|
  | 0xc1df5e2e | store/purchased | 0x5e7fc0, 0x62ea60, 0x62ef70, 0x62fb60, 0x630580, 0x76bbf0 |
  | 0x6e9d9193 | gradient-bar | 0x5da090 |
  | 0xd41f59a1 | medal-sheen | 0xc24df4 |
  | 0x7f89e267 | up-button-pulse-big | 0xc45e20 |
  | 0xd75be949 / 0x576ed245 | mode-buttons question-mark / notification | 0x64c510 |

  The ELF also contains store/locked and trophy outline/solid/blank sids.
* Material lookup `0x11b89b0(u64 key = sid | table<<32, index)`:
  * table 1 = UI+0x28 (net) and 2 = UI+0x30 (menu), both searched by sid;
  * table 0 = UI+0x20 (hud) is accessed by index only; callers find the index by a linear sid search (e.g. 0x62eb38).

### What the merge does
* Pages 0 and 1 and the VRAM data are copied from retail **byte-for-byte**. All 12630 retail fixups, the item list, names and sids are unchanged.
* The only retail bytes changed are GEOMETRY_1 data +0x48 (material count 205 -> 390) and +0x50 (material array pointer). The pointer now targets page 2; its fixup changes from dst page 0 to 2.
* New page 2 holds 3 RAW_DATA items:
  * alias name strings;
  * private copies of the binding arrays (the runtime writes the texture index/hash into bindings);
  * the material array: the 205 retail entries in their original order, so indices are stable, plus 185 aliases.
* Each alias = the retail colon material's shader, params, effect hash and bindings (pointing at retail strings and textures), under the PS3 name. Aliases are built only for PS3 sids that are absent. `--all-slash` also aliases the 20 PS4-only (SP/photo-mode) materials; this is not used by default.
* No new textures or effects are needed; all of them already exist in the retail pak.
* **One GEOMETRY_1 only.** A background GEOMETRY_1 login (0x10f6700 / 0xf48060) reuses level+0xf0, and ConstructFromBoundMaterialTable (0xe4bf60) asserts `m_numMaterials == 0` and `m_aMaterial == NULL` on level+0x150. A second GEOMETRY_1 in the same level would trap, so the material array is extended instead.
* Login order (EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1, TAG_FLOAT) is unchanged.
* Output: `out/hud.pak`, 29,886,064 bytes (retail 29,811,360), 3 pages, 15681 fixups.
  * `ps4check`: OK, 390 materials and 778 bindings resolved.
  * Checked by script: the first 205 materials are identical to retail, and all 185 aliases match their source except for the name.

### Emblem quad material
* The PS3 emblem renderer 0x27aa4c (spray-01 / emblem-16-drips) picks one of two materials, both in **hud.pak**:
  * `general/hud/alpha-mask-icon/default` (0xa0cf6221) when a layer has a mask;
  * otherwise `general/hud/alpha-only-icon/default` (0xd2e394f5).
  * The key is built via 0x97202c and the lookup is 0x971f64.
  * It then draws via 0x96b0c4 with the layer texture (r6 = tex+16) and an optional second texture, the mask (r7 = tex+16 or 0).
* PS3 texture slots:
  * alpha-only-icon: {default-color-white, default-trans}
  * alpha-mask-icon: {white, default-trans, <unbound runtime slot>}
* PS4 retail has both under colon names, and neither sid occurs in the PS4 ELF, i.e. the PS4 build removed the PS3 emblem renderer:
  * `alpha-only-icon:default` (0x4edc816b): ND_MENU3D + ND_TRANSPARENCY; ColorMap + TransparencyMap
  * `alpha-mask-icon:default` (0x3cf077bf): ND_MASK, ND_MENU3D, ND_TRANSPARENCY; Color + Transparency + MaskMap (bound to default-trans)
* The material with a single texture is `alpha-icon:default` (0xf518a1aa, ColorMap only, ND_TRANSPARENCY2), used by photo mode 0xc3f840. It is the likely cause of the earlier "missing second texture" crash.
* The merged pak now also answers the PS3 sids 0xd2e394f5 and 0xa0cf6221. An emblem helper should use those (or the colon sids) with table 0.

### Risks
* Not tested in game.
* The 185 aliases add 185 bound-material / effect-binding jobs at hud login. They are small, but memory use rises.
* Aliases share the retail param lists and effect strings, which are read-only. Binding arrays are private copies.
* The hud table on PS4 is reached by index. Retail indices 0..204 are preserved, so any cached index stays valid.
* The overlay must shadow the retail `pak23/ps4/hud.pak` with this file; hud-ingame12.pak is unchanged.

---
## 7. Boot crash with merged hud.pak, and `out/hud-min.pak`

### What the crash actually is
* The fault at 0x12c4917 is `movzx edx, WORD PTR [rsi+2]` with `rsi = *( *(0x1b24b80+0x48) + 0x20 )`, i.e. the **hud material table pointer (UI+0x20) is NULL**. It is not a NULL object from the 0x2c80 allocation: rbx is cleared by `xor ebx,ebx` at 0x12c4903, and rax = rdi = 0xe887da64 = sid("general/hud/bp-speedrun/bp-clock:default"), the lower-cased string it was about to search for.
* Boot order (0x569ac0):
  1. 0x56b1ea calls 0xc18900, which requests misc-fx, misc-fx-fma, decals, particle*, particle-override and then **hud** (callback 0xc18df0).
  2. 0x56b1ef calls 0x12bdef0 (Bluepoint UCC tweaks). It allocates 0x2c80 bytes and calls 0x12c4420, the speedrun clock init. That function unconditionally searches the hud table for bp-clock.
* 0xc18df0 runs only when the hud level is loaded and its level+0x150 table exists. It hands the table to the UI object (0x1805fa0 vtable+0x10 — this is what fills UI+0x20) and resolves pickup-indicator ids via 0xe4be10.
* With retail hud.pak the table is set by then. With the merged pak it was not, so **the hud level did not finish loading/logging in before this point.**

### Most likely mechanism: login-data allocations
* Everything allocated during login and the geometry/material jobs comes from the per-package `LevelMemAlloc`:
  * 0xdeba10 pushes it;
  * Allocate 0xdeb5e0 calls 0xdd7910, which calls Package::AllocateLoginData 0xdd9d20.
* AllocateLoginData first uses free tails and adjacent free 32 KiB sub-pages of the package's own loading-heap pages. Failing that, it takes a free run elsewhere or a whole 2 MB heap page (0xdcf3b0). If that also fails it asserts "Level login allocation has failed" (allocator-levelmem.cpp:0x4e) and traps (int 0x41).
* Per-package page slots (0x4b0) and the shader pool (dedup by hash, capacity 0x2000) are not limits here.
* Login data grows linearly with material count:
  * 0xf63000: 0x70·N bound materials (43,680 B for 390, i.e. more than one sub-page) and 0x20·N;
  * 0xe8e5a0: per material and technique 0xa0+0x90+0x90 bytes plus an ndgi object;
  * 0xe4bf60: 0x28·N plus per-material texture lists.
* The merged pak added 185 materials and a new 50 KB page at boot, when several packages compete for the loading heap. A failed or trapped login leaves hud unloaded, so UI+0x20 stays NULL and the Bluepoint init faults.
* Not runtime-verified. If the log shows "Level login allocation has failed" or "Allocation failed: ... LevelMemAlloc", that confirms it.

### `out/hud-min.pak` (29,834,848 bytes, 2 pages)
* **14 materials renamed in place:** ':' becomes '/' in the material name string *and* in the matching submesh material-name string (same length, so no layout or size change). These are the ones whose colon-form sid is referenced nowhere in the ELF or the staged DC files (net / menu / modules):
  * gradient-bar, up-button-pulse-big, medal-sheen
  * trophy outline / solid / blank
  * mode-buttons question-mark / notification / warning / check-mark
  * flag-icons/treasure, treasure-chest
  * alpha-only-icon (0xd2e394f5) and alpha-mask-icon (0xa0cf6221)
* **2 aliases:** `general/hud/store/purchased` (0xc1df5e2e) and `store/locked` (0xea36b17f). Their colon sids 0x658daf73 / 0x7a8f9351 are still searched by PS4 code at 0x5b2740, so both names must exist.
  * The material array (205 retail entries in order, plus 2) is appended **to page 1**. It grows from 0xb80 to 0x4695 bytes, still inside its single 32 KiB sub-page, so no new loading-heap sub-page is needed.
  * GEOMETRY_1 +0x48 goes from 205 to 207, and +0x50 points to page 1. Page 1 is not freed after login: the loading-heap free calls are header/FreeLoginPages, ReleaseLoadedVramPages and Unload.
* **Retail delta:** page 0 has 28 renamed bytes plus the 2 geometry fields; page 1 has the appended data plus its size field. VRAM and all retail fixups are identical except the redirected +0x50. All 185 submeshes still resolve to a material.
* **Extra login data:** 2 materials (about 1% of the hud set). Bound-material array 0x70·207 = 23,184 B, which fits in one sub-page.
* `ps4check`: OK, 207 materials and 413 bindings. All 16 slash sids and both colon store sids resolve.
* **Remaining risk:** a colon-form name referenced by data that was not staged (hud DC, other `bin.psarc` modules) would no longer resolve for the 14 renamed entries. If that shows up, move that sid to the alias list (`merge_hud.py --minimal`, `MIN_SIDS`).

Tools:
* `merge_hud.py --minimal --scan <elf> <dc bins>` builds hud-min;
* `--min` aliases MIN_SIDS by appending to page 0, which adds 1 sub-page;
* the default mode is the full 185-alias build that crashed.

## 8. What is missing on PS4 (inventory, 2026-10-26)

- Actor packages: PS3 disc + 1.19 patch + DLC have 2758; PS4 has 2138. **679 missing**: 190 disc (custom-kit parts, co-op characters, accessories, MP map props/anims), 294 patch13 (1.13 accessories, cowboy kits, shirts, treasure items), 135 patch18 (masks/hats/shirts + 45 `multi-celebrate-*` taunts), 60 DLC map props/anims.
- Levels: PS4 keeps only MP sky domes, cargo-plane and a few `-multi` stubs. **All base MP maps and co-op maps are missing** (144 level definitions, 223 of 254 MP packages).
- Taunts: PS4 net.bin `0xb2b6e512` has the 15 disc taunts ({u32 anim sid, u32 text, u32 icon}); 1.19 has 44 (anim name as string). The 15 disc taunt anims are inside PS4 `anim-proto-multiplayer.pak`.
- MP hero names in net.bin: PS4 119, 1.19 330 (accessory variants).

## 9. PS3 foreground mesh format — decoded and verified (`ps3geom.py`)

Verified against PS4 versions of mp-hero-ramses-bandana, mp-kit-base-head-skin-a and mp-hero-talbot-torso: 42 segments, positions (<1e-4), UV, normal, tangent, triangle lists (exact) and skin weights (exact) all match. Vertex and triangle order are identical across builds.

- PM_FG_PROTOTYPES -> prototype (+6 u16 submesh count, +0x14 block table). FOREGROUND_SECTION_2 item -> prototype.
- One block per PS3 segment; PS4 concatenates consecutive segments of the same material into one submesh (in block order).
- Attributes are separate MSB-first bit streams. Fixed point `0x4N`: `a*(1+v*2^-21)+b`; `0x6N`: `a*(1+(4v+I)*2^-23)+b`; signed normalised `0x0N`: `v*2/(2^bits-1)-1`.
- Index compression: header {bits, nExplicit, nSlots, start}; 2-byte aligned streams: explicit indices, 2-bit configs (0: (p0,p2,new), 1: (p2,p1,new), 3: three new), 1-bit slot flags (0 = running counter).
- Skinning: palette = concatenated {first joint, count} ranges; vertices in groups of 4; per-group code byte (4/6 shared list, 2/10 per-lane rows (lane order 1,0,3,2), 0 repeat previous group, 8 new weights, 6/8/10 carry u8 weight rows); joint entries are `palIndex*48 (+4 = more follow)`.

PS4 target layout (GEOMETRY_1 v0x10, foreground): submesh 0x88 {name, flags, nverts, nindices, nstreams, streams*, index RAW_DATA*, material name, skin*}; stream 0x18 {u16 1, u16 stride, u32 size, fmt*, data*}; fmt {slot, 0, size, type (6 float3, 0x22 half2, 0x0a half4)}; slots 0 pos, 1 uv0, 2 normal(+w 1), 3 tangent(+w sign); skin 0x28 {0, total, counts u16*, offsets u16*, joints u16*, weights f32*}. PS4 adds a `lib-shadowblocker` submesh per prototype (PS3 has it too).
