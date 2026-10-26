# U3 MP menu helpers: PS3 to PS4 map

Scope: every PS3 function called directly by f_33b574, f_331e6c, f_338ab8, f_33a3b8, f_340bd0, f_346e4c (/tmp/p60) and f_34634c, f_28d358, f_29366c, f_285bc4, f_283828, f_292e3c (/tmp/p61). That is 177 callees.

Method: I checked each pair structurally with Ghidra decompiles on both sides. The checks were shared hash/text-id constants, assert strings, field offsets (allowing for 32 to 64-bit pointer growth), callee pattern, and call position inside a datasource pair that survives in both builds (mainly f_339698/0x7680f0, f_34c45c/0x773b50 and f_3481c0/0x759aa0). `pairs.json` was wrong for about half of the entries I checked. For example, it swaps f_38be9c and f_38c9f0, and maps f_9a77a4 and f_355934 to the wrong functions.

Confidence levels:
- **verified**: logic, constants and offsets match, or the call sits in the same position inside a verified pair.
- **likely**: consistent evidence, but not every branch was compared.
- **unknown**: not located, or not investigated.

"inline" means PS4 has no callable copy, so you have to reproduce the code; the pattern to use is given in the row. "none (stripped)" means the only PS3 callers are functions that are RET stubs on PS4, and no PS4 code with that behaviour could be found. The linker probably dropped it.

Counts: 177 callees in total.
- **Priority A** (73 helpers): 44 verified, 19 likely, 10 unknown. All 10 unknowns are party/NP/voice helpers of f_34634c.
- **Group B** (20 helpers): 4 verified, 6 likely, 10 unknown.
- **Group C** (84 helpers): 17 verified, 17 likely, 50 unknown. Most of Group C was not investigated.

## Datasource ABI changes (from f_339698 vs 0x7680f0)
- **Event codes:** shifted by +2, except init, which stays 9. The mapping is PS3 0 (text) -> 2, 2 (icon) -> 4, 3 (get selection) -> 5, 4 (count) -> 6, 8 (set selection) -> 10 (0xa), 10 (size) -> 0xc, 0xf -> 0x11, 0x14 (activate) -> 0x16.
- **param_1 fields** (u32 index):
  - [1] stays [1].
  - [0xd] (text char*) becomes a u64 at [0x10].
  - [0x10] (column hash) -> [0x14]; [0x11] -> [0x15]; [0x13] (activated row) -> [0x17]; [0x21] -> [0x28]; [0x1b]/[0x1c] (sizes) -> [0x1f]/[0x20].
  - Icon block: PS3 [5..7] -> PS4 u64 at [7] = (iconId | 0x100000000) plus [9] = icon index; PS3 [9] (colour) -> [0xb]; [10] (alpha/scale) -> [0xc]. [2] (type) is unchanged.
- **Current user:** `u = DAT_017bde75 ? 1 : DAT_017bf714` (PS3: 0x1215905 / 0x12167a0).

## Struct layout changes found
- **Screen/profile object:** PS3 *(0x1248638)+u*0x7980 becomes PS4 *(0x17dd610)+u*0x7b00.
  - Lazy-init state +0x7968 -> +0x7ae8.
  - Profile data base obj+0xc8 -> obj+0x248. Offsets relative to the data base are unchanged. Examples: money data+0x504 -> obj+0x74c; level data+0x500 -> obj+0x748; flags data+0x12e8 -> obj+0x1530; coop mod bytes data+0x12ec -> obj+0x1534; completed-mission bits data+0xc84 -> obj+0xecc; active mission data+0x161c -> obj+0x1864; unlock bits obj+0x1290/+0x13c4 -> obj+0x1410/+0x1544.
  - Loadout data at data+8+(loadout*0xc+field)*4 (fields: 4..6 boosters, 7 kickback, 8/9 primary weapon / mods, 10/11 mod masks).
  - Current loadout byte obj+0x797c -> obj+0x7afc; table 0x1248640 -> 0x17dd618.
  - PS4 runs the lazy default-init inline before every access: if state==3 && !0x17d5c61 && 0x17d5c80, then memset(obj+0x24c,0,0x2824), set the defaults, and set state=2.
- **Unlock manager** (0x125f1b8 -> 0x17f3068, object 0x118 -> 0x120 bytes):
  - Cache fields +0x100 table / +0x104 version / +0x108 package id / +0x10c package ptr -> +0x100 (8 bytes) / +0x108 / +0x10c / +0x110.
  - Entry 0x1c -> 0x28 bytes: {+0 bit, +4 type, +8 itemId, +0xc cost, +0x10 subId, reqs ptr +0x14 -> +0x18, dlc +0x18 -> +0x20}. Requirement records stay 0xc bytes.
- **Booster manager** (0x121b938 -> 0x17c3b40): +8+slot*4 (equipped override) and +0x18+slot (cached level) are unchanged. Cache +0x1c/+0x20/+0x24/+0x28 -> +0x20/+0x28/+0x2c/+0x30. Entry 0x2c -> 0x38: {+4 id, +0xc slot/type, +0x10 nTiers, tiers ptr +0x14 -> +0x18, cost +0x18 -> +0x20, flags +0x28 -> +0x30}. Tier record 0xc.
- **Booster list** (f_32cb90 / 0x7596a0 output): count +0x104 -> +0x208; entry ptr[] +4 -> +8, so 0x108 -> 0x210 bytes per list.
- **Kickback table** (statics 0x123069c..0x12306a8 -> 0x1b6a3a0..0x1b6a3b0, package hash 0xd9f6ada2): entry 0x1c -> 0x28, tiers ptr +0x14 -> +0x18, tier 0x14 -> 0x18. Per-user kickback object 0x12306ac+u*0x20 -> 0x17d79f0 (user at +0xc).
- **Mission/challenge manager** ("weapon-mod data" 0x1233980 -> 0x17da200): stride 0xa14 -> 0x1018; count +0xa00 -> +0x1000; user +0xa04 -> +0x1004; list entry 0x14 -> 0x20 ({id, ptr@+8, +0x10, +0x14, flags@+0x18}). Def-table statics 0x1236dac..0x1236db8 -> 0x1b6d360..0x1b6d370.
- **Menu manager** (0x12151a0 -> 0x17bd080):
  - +4/+8 state -> +8/+0xc.
  - Pages +0x2f0[0x84] -> +0x390[0xa0]; current popup +0x504 -> +0x898; +0x50c -> +0x8a8; stack +0x510[] -> +0x8b0[]; depth +0x720 -> +0xdb0.
  - Dialog block ptr +0xcac -> +0x1438 (=DAT_017be4b8); wait-popup handlers +0x1364/+0x1368 -> +0x2160/+0x2168.
  - Page id: page+0xc -> page+0x10.
- **Dialog block** (f_26a364): every field moves by +4 (+4 -> +8, ... +0x28 -> +0x2c).
- **Weapon preview scene** 0x120ee80 -> 0x17bba90: +0xcdc -> +0x15dc; preview handle +0xbb4/+0xbb8 -> +0x1490/+0x1498 (abs 0x17bcf20/0x17bcf28); preview object mask/index +0x6f0/+0x6f2 (PS4).
- **Net game manager** 0x1223da0 -> 0x17cb060: current mode short +0x5948 -> +0x661c; mode table symbol 0x3b9a067d stride 0xb8 -> 0xd0; +0x5c08 -> +0x6a24. Map symbol 0xdec97a4 stride 0x54 -> 0x98. Playlist symbol 0xc1b0be0d stride 0x5c -> 0x68.
- **Party** 0x1237bb0 -> 0x17dc298 (second instance 0x122ec60 -> 0x17d6420): +0x10 -> +0x18; members +0x670 stride 0x180 -> +0x680 stride 0x120; +0x16f4 -> +0x12e4; +0x1700/+0x1704 -> +0x12f0/+0x12f4.

## Globals
| PS3 | PS4 | meaning | evidence | conf |
|---|---|---|---|---|
| 0x125f1b8 | 0x17f3068 | unlock manager | arg of 0x7bb470/0x7bd6f0/0x7bda50 in 0x7680f0 | verified |
| 0x121b938 | 0x17c3b40 | booster manager | arg of 0x6882b0/0x687d30/0x6890f0 | verified |
| 0x11f47c8 | none needed | money/xp "manager" (stateless this) | PS4 0x662760 ignores param_1 (callers pass garbage); money/level read inline from the profile | verified |
| 0x1233980 | 0x17da200 (+u*0x1018) | mission/weapon-mod data | 0x6882b0, 0x74eba0 call 0x7716c0(&0x17da200+u*0x1018) | verified |
| 0x1232a80 | 0x17d9988 | current loadout index (ds state) | f_355f5c(..,DAT_01232a80) == 0x7800d0(..,DAT_017d9988) in weapon mods | verified |
| 0x1232a84 | 0x17d998c ? | kickback selection (ds state) | only PS4 ref is in 0x608fd0; sits between the a80/a88 counterparts | likely |
| 0x1232a88 | 0x17d9990 | booster slot (ds state) | f_2851d4 vs 0x66fff0 (`==3` test) | verified |
| 0x1232a9c | 0x17d9994 | primary/secondary flag | weapon mods / sub weapon pair | verified |
| 0x1232aa0 | 0x17d9998 | coop weapon index | f_338a18 vs 0x767a60; 0x5b3e40 | verified |
| 0x1233204 | 0x1b6ae68 | weapon-mods selection | 0x7680f0 | verified |
| 0x1232bd0 / 0x1232be0 | none | booster ds selection[user] / per-slot list (0x108 bytes each) | only referenced by stub f_33b574; allocate your own (PS4 list layout is 0x210 bytes, see above) | likely |
| 0x1232ac0, 0x1232a90, 0x1232a98, 0x12332a8.., 0x123332c, 0x123384c, 0x123398c | none | per-datasource statics of stubbed datasources | no PS4 refs found | likely |
| 0x1218300 | 0x17c23a0 | menu "active user" index | f_284998 vs 0x6731b0, f_2851d4 vs 0x66fff0 | verified |
| 0x1218304 | 0x17c23a8 | object with vtable (slot 0 = bool) | f_285530 vs 0x66f860 | verified |
| 0x1215905 / 0x12167a0 | 0x17bde75 / 0x17bf714 | split-screen flag / primary user | all datasources | verified |
| 0x12151a0 | 0x17bd080 | menu manager | 0x651940 arg | verified |
| 0x12151b0 | (menu+0x10) | passed to f_253bdc/f_2548b4 but ignored by both | PS3 disassembly | verified |
| 0x120ee80 | 0x17bba90 | 3D weapon-preview scene | 0x5b3e40 call at 0x766e81 | verified |
| 0x125f2d0/2d4/2d8/2dc/2e0/2e4 | 0x17f3188/318c/3190/3194?/3198/319c | purchase-dialog context (item, sub, user, page, row, ctx) | case 0x14 (PS3) vs 0x16 (PS4) of weapon mods | verified (2dc likely) |
| 0x125f304 | not needed | "this" of f_38de20 (0x7bea40 ignores it) | | verified |
| 0x12f1e98 | 0x17026b8 | localized text DB (fallback string at +0x10 = 0x17026c8) | text lookups | verified |
| DAT_012f1f2c+0x21f4 | *(0x1b24b80)+0x4c7c | "show string ids" debug flag | f_799f8c inline | verified |
| 0x1248638 / 0x1248640 | 0x17dd610 / 0x17dd618 | screen object array / per-user loadout index | profile getters | verified |
| 0x1223da0 | 0x17cb060 | net game manager | 0x7270f0 arg | verified |
| 0x122d650 block: +4 (0x122d654), +5 (0x122d655), +0x24, 0x122da24, 0x122da4d | 0x17d5c58 block: 0x17d5c60, 0x17d5c61, 0x17d5c80, 0x17d603c, 0x17d6065 | session state flags (0x17d5c61 = online, 0x17d5c80 = profile-ready) | used identically in many pairs | verified |
| 0x11ce9e4 | 0x17b2a94 | "everything unlocked" debug flag | 0x7bba70, 0x74eba0 | verified |
| 0x121b6e8 | 0x17c3930 (DLC owned bytes at +0x110 = 0x17c3a40) | DLC manager | f_29c500 inline | verified |
| 0x125ef1c | 0x17f2f98 (+0x28 = 0x17f2fc0) | lobby state | f_383ff8 inline in 0x7a2b10 | verified |
| 0x12307ba | 0x17d7b06 | flag | f_383ff8 inline | verified |
| 0x01230770 | 0x17d7abc | level override | f_5ba34 vs 0x662760 | verified |
| 0x1237bb0 / 0x122ec60 | 0x17dc298 / 0x17d6420 | party objects | args of 0x7a2b10/0x7a3400 | verified |
| 0x11d1118 (+0x2208, +0x21bc) | 0x17b9ca0 / 0x17b9d10 | title id string / hash | f_44718 inline | verified |

## Surprising findings
1. **f_799f8c is inlined.** 0xf32ae0 is the raw table lookup (it equals f_799ed8, which is recursive). It returns NULL on a miss. Use the full inline pattern from the table for identical behaviour.
2. **pairs.json swaps f_38be9c and f_38c9f0.** The correct pairs are f_38be9c = 0x7bd6f0 (can buy) and f_38c9f0 = 0x7bda50 (sets the purchased bit and notifies).
3. **Helpers used only by the stubbed datasources were stripped from PS4,** not just inlined. Examples: f_29dea0 (equip booster + network sync), f_328a40 (buy/equip kickback), f_79aafc ([TEXT2]/[TEXT3] formatter), f_322ccc. You have to reimplement them from the PS3 code.
4. **The PS4 binary has a separate, function-pointer-driven loadout/lobby UI** in 0x605000-0x61a000. Its vtables are at 0x1679920, 0x1679a00, 0x167a300 and 0x167a420, for example 0x6053c0, 0x60bec0, 0x60ddb0, 0x608fd0. It uses the booster/kickback helpers (0x687d30, 0x6882b0, 0x74fc10, 0x74e6d0) and booster/kickback icon ids (0xd8b04236, 0x19d9ea44). It is the only code that touches 0x17d998c. It may be worth studying as a PS4-native reference for the booster/kickback screens.
5. **Facebook, avatar-icon and NetPlayerData-publish strings are absent from the PS4 binary** ("Facebook SignIn", "Need to retry %s Load Icon", "NetPlayerData Publish Thread", "Select Second User", "Send Message %x"). The party list's NP/voice helpers have no 1:1 counterparts.
6. **Some getters exist out-of-line on PS4 even though most callers inline them.** f_355f5c = 0x7800d0, f_355e94 = 0x780220 and f_309bbc = 0x722670 are real functions.

## Extra mappings found on the way (callees of callees)
| PS3 | PS4 | what | conf |
|---|---|---|---|
| f_38b508 | 0x7bb5b0 | unlock requirement check (type 1..7) | verified (same position in 38be9c/7bd6f0, 38b390/7bba70) |
| f_38b934 | 0x7bcda0 | find requirement record | verified (position in 38cde0/7bca00) |
| f_38a5a0 | 0x7bcf40 | scaled cost (symbol 0x4d466631) | likely |
| f_38c280 | 0x7bd0b0 | on-unlock notify | verified |
| f_29d0a4 | inline | booster equipped id (see f_21724c) | verified |
| f_278098 | inline 0x7be0b0(m,u,0x7bb470(m,id,0)) | booster unlocked-for-list check | verified |
| f_2069b4 | 0x5b3e40 | weapon-preview update | verified |
| f_6d2e88 | 0xd49270 | preview object refresh | likely |
| f_93e934 / f_2060ac | 0x11a6250 / 0x5b4010 | preview release / spawn | likely |
| f_341bbc | 0x751aa0 | set page selection (symbol 0xf1514340) | verified |
| f_264d8c | 0x6531f0 | menu page transition | likely |
| f_29c510 | inline (symbol 0xf5899ce4, 0x18-byte entries, +0x14) | DLC entry | likely |
| f_30469c | 0x73fbc0 | playlist allows boosters | likely |
| f_34abd8 | 0x771280 | mission def lookup | likely |
| f_34b5bc | 0x7716c0 | mission tiers completed | verified |
| f_29da44 | 0x688ee0 | booster modifier list | verified |
| f_355934 | inline | profile data | verified |
| f_d646e8 | 0x139c740 | strncpy | verified |

### Priority A: f_33b574 (boosters), f_331e6c (medal kickbacks), f_338ab8 (coop weapon), f_33a3b8 (coop weapon mods), f_34634c (party list)

| PS3 | PS4 | what it does | evidence | conf | called by |
|---|---|---|---|---|---|
| f_5b5a8 | inline | money(user) = data+0x504 | PS4 inline *(u32*)(obj+0x74c) after lazy-init (seen in 0x7680f0, 0x7bca00, 0x6890f0) | verified | f_33b574 f_331e6c f_33a3b8 f_346e4c |
| f_21724c | inline | booster: equipped entry for (user,slot,loadout) = f_29d33c(m, f_29d0a4(...)) | PS4: id=*(0x17c3b48+slot*4) ?: 0x7800d0(obj,slot+4,loadout) (slot==3: *(mgr+u*4)); then 0x687d30(&0x17c3b40,id) (seen 0x6b41e0) | verified | f_33b574 |
| f_24a90c | 0x62aa60 | sprintf into static buffer | given anchor | verified | f_33b574 f_331e6c f_33a3b8 f_340bd0 f_346e4c |
| f_253bdc | inline | set preview mod mask/idx on preview object | PS4: obj=handle(0x17bcf20,0x17bcf28 via +0x7c id); *(u16*)(obj+0x6f0)=mask; *(u8*)(obj+0x6f2)=idx; 0xd49270(obj) | verified | f_33a3b8 |
| f_2548b4 | inline | refresh 3D weapon preview: f_2069b4(scene, mode==-1, mode==1, arg) | PS4 calls 0x5b3e40(&0x17bba90, 0, 1/..) directly (0x766e81) | verified | f_338ab8 |
| f_260bf8 | 0x651fa0 | play UI sound (idx) | 13 identical sound-hash consts | verified | f_33b574 f_33a3b8 f_346e4c |
| f_262894 | inline | top-of-stack page id | PS4: d=*(menu+0xdb0); d>=0 ? *(u32*)(*(menu+0x8b0+d*8)+0x10) : 0 | likely | f_346e4c f_34634c |
| f_269bfc | 0x651940 | show page | given anchor | verified | f_338ab8 f_340bd0 f_28d358 f_29366c |
| f_26a364 | inline | push confirm dialog then show page 0x719e9fec | PS4: p=*(0x17be4b8) (=menu+0x1438); fields +4/+0x10/+0x14/+0x18/+0x1c/+0x20/+0x24/+0x28 -> +4 each; 651940(&0x17bd080,0x719e9fec,0,0) | verified | f_33b574 f_33a3b8 f_346e4c f_28d358 f_29366c f_285bc4 f_292e3c |
| f_29cc48 | inline | booster: tier entry i (if i<+0x10) = *(+0x14)+i*0xc | tiny accessor, no PS4 fn; PS4 entry tiers ptr moved +0x14->+0x18 (8-byte ptr) | likely | f_33b574 |
| f_29cd8c | inline | IsUnlocked(user, find(a,b)) = f_38b390(m,u,f_38b054(m,a,b)) | PS4 emits 0x7bba70(&0x17f3068,u,0x7bb470(&0x17f3068,a,b)) (0x6882b0, 0x766870) | verified | f_33b574 f_331e6c |
| f_29cde8 | 0x6882b0 | booster: current tier/level (user, booster entry) | same: mgr+8+slot*4 equipped id, mgr+0x18+slot level, IsUnlocked, min(f_34b5bc+1) | verified | f_33b574 |
| f_29cf10 | inline | booster: tier entry for current level = f_29cc48(f_29cde8) | only caller f_33b574 (stub); compose 0x6882b0 + accessor | likely | f_33b574 |
| f_29d204 | 0x6890f0 | booster: any network booster affordable (type 3, cost<=money) | f_30469c->0x73fbc0, f_30b3a4->0x7270f0, money +0x74c, entry +0xc==3, cost +0x18->+0x20, stride 0x38 | verified | f_33b574 |
| f_29dea0 | none (stripped) | booster: equip/unequip + network sync | only callers f_28dbf8/f_33b574 both RET stubs on PS4; no fn writes 0x17c3b48[] with 2edcbc/38e358 pattern | likely | f_33b574 |
| f_30089c | unknown | find net player by id (net+0x1f0, stride 0x8b0) | not located (170 callers on PS3; likely inlined) | unknown | f_34634c |
| f_301214 | unknown | find net player by NP id hash (0x1223f90 array) | not located | unknown | f_34634c |
| f_307728 | 0x721610 | current game-mode type | symbol 0x3b9a067d, short +0x5948->+0x661c, stride 0xb8->0xd0, +0xc | verified | f_34634c f_292e3c |
| f_3082a4 | 0x729b10 | mode allows X (types 0,1,19 / 0xf with lobby -0x2c4792a0) | mask 0x80003, 0xf check, -0x2c4792a0 | verified | f_34634c |
| f_309bbc | 0x722670 | is ranked/team mode (types 4,8,9,11,13,16,17) | mask 0x72b1 over type-4; also inlined widely | verified | f_34634c |
| f_30a978 | 0x7418e0 | player name color/clan tag struct (out,net,idx,flag) | same init-once statics (0x122af00->0x1b69378), mode 0xe check, 4 args | likely | f_34634c |
| f_30b3a4 | 0x7270f0 | online features allowed (game-mode + title-id checks) | same mode mask (0x72b1), 0x12 check, title-id strcmp, +0x5c08->+0x6a24 | verified | f_33b574 |
| f_30b5ec | 0x7210a0 | mode check (309bbc && type!=0x12 ...) | same structure | verified | f_34634c |
| f_31c2e8 | inline | net state *(0x122d650+0x3c) != 0 | 1-instruction accessor; PS4 0x122d650 block -> 0x17d5c58 (+0x3c -> ?) | likely | f_34634c |
| f_322ccc | none found | if flag && menu idle: show page 0xe35f0f1a + f_341bbc | PS4 0x66ed20 shows same page but is f_282de4; f_322ccc only called from f_33b574 (stub). Use 0x651940 + 0x751aa0 | likely | f_33b574 |
| f_328158 | inline | kickback: equipped id = f_355f5c(obj(u@+0xc),7,loadout) | PS4: 0x7800d0(*(0x17dd610)+*(kb+0xc)*0x7b00,7,loadout) (e.g. 0x74e6d0, 0x607f97) | verified | f_331e6c |
| f_328254 | inline | kickback: n-th entry with flag (1 or 2) | no standalone fn in PS4 kickback cluster 0x74e4f0..0x74fc10; callers only f_331e6c (stub) | likely | f_331e6c |
| f_32834c | 0x74e5a0 | kickback: find entry by id | same static cache 0x123069c..a8 -> 0x1b6a3a0..b0, stride 0x1c->0x28 | verified | f_331e6c |
| f_328414 | inline | kickback: count entries with flag | no standalone fn; only caller f_331e6c (stub) | likely | f_331e6c |
| f_328a40 | none (stripped) | kickback: equip / buy dialog | only caller f_331e6c (stub); no PS4 fn found writing loadout field 7 with this flow | likely | f_331e6c |
| f_328ccc | 0x74eba0 | kickback: current level (min(tiers-1, f_34b5bc+1)) | identical logic, 0x11ce9e4->0x17b2a94, calls 0x7716c0 | verified | f_331e6c |
| f_328dfc | 0x74fc10 | kickback: tier entry for current level | entry tiers ptr +0x14->+0x18, tier stride 0x14->0x18, calls 0x74eba0 | verified | f_331e6c |
| f_32c984 | inline | status(user, find(a,b)) = f_38cde0(m,u,f_38b054(m,a,b)) | PS4 emits 0x7bca00(&0x17f3068,u,0x7bb470(&0x17f3068,a,b)) (LIST_PRIMARY_WEAPON_MODS @0x7680f0) | verified | f_33b574 f_331e6c f_33a3b8 |
| f_32cb90 | 0x7596a0 | build booster list for slot (symbol 0x916e03b0) | same: count +0x104->+0x208, ptr[] +4->+8, entry +0x28->+0x30 flags; f_278098 inlined as 0x7be0b0(0x7bb470) | verified | f_33b574 |
| f_32d2d0 | none found | party list build (0x1233234..) | only caller f_34634c (stub) | unknown | f_34634c |
| f_32d4d8 | 0x751b30 | weapon-mod description/price text (item, mod) | 4 shared text-ids (0x204bac9d,0x577cf7a5,0x29385507,0x6ba044a1); same call slot in LIST_PRIMARY_WEAPON_MODS | verified | f_33b574 f_331e6c f_33a3b8 |
| f_345c98 | none found | party list sub-handler | only caller f_34634c (stub) | unknown | f_34634c |
| f_34aa30 | inline | mission progress value for id | inlined in 0x7733c0: id<0x100: active(obj+0x1864)==id ? obj+0xf4c : 0; else list entry(+0x20 stride, flag bit0) +0x10 + +0x14, else obj+0x250+(id+0x249)*4 | likely | f_33b574 |
| f_34b1ac | 0x775d30 | mission: next incomplete tier for id | via pair f_3481c0=0x759aa0 (calls 6882b0+775d30); static cache 0x1236dac..b8 -> 0x1b6d360..70 | verified | f_33b574 |
| f_355c04 | inline | xor byte data+0x12ec+idx | PS4: *(u8*)(obj+0x1534+idx) ^= v; no standalone fn | likely | f_33a3b8 |
| f_355c54 | inline | get byte data+0x12ec+idx (coop weapon mod mask) | PS4: *(u8*)(obj+0x1534+idx) (data base 0x248); no standalone fn | likely | f_338ab8 f_33a3b8 |
| f_3742d0 | inline | member id == party+0x16f4 | PS4 field +0x12e4 | likely | f_34634c |
| f_3749ec | unknown | member voice/status icon (f_253b5c) | only caller f_34634c (stub) | unknown | f_34634c |
| f_374a3c | 0x7a3510 | party member display name "[%s] %s" | identical format strings "[%s] %s%s(1)" / "[%s] %s(1)" | verified | f_34634c |
| f_374c00 | inline | party member by name (single loop) | no standalone; strcmp loop over +0x680 stride 0x120 | likely | f_34634c |
| f_3757c8 | 0x7a2b10 | party member count | same: +0x10 ptr->+0x18; f_383ff8 inlined; member stride 0x180->0x120 | verified | f_34634c f_29366c f_292e3c |
| f_375fb0 | 0x7a3400 | party member by name (or self) | same two loops, strcmp=0x139c710, +0x16f4->+0x12e4, +0x1700/+0x1704 -> +0x12f0/+0x12f4 | verified | f_34634c f_292e3c |
| f_383ff8 | inline | is in online game lobby (0x122d655 && !0x12307ba && *(0x125ef1c+0x28)==3) | PS4: 0x17d5c61 && !0x17d7b06 && 0x17f2fc0==3 (0x7a2b10, 0x6534e0) | verified | f_33b574 f_29366c f_292e3c |
| f_38b054 | 0x7bb470 | unlock mgr: find unlock entry by (itemId, subId) | same table walk (+0x100 tbl/+0x108 pkg id), entry stride 0x1c->0x28, match +8/+0x10 | verified | f_33b574 |
| f_38b390 | 0x7bba70 | unlock mgr: IsUnlocked(user, entry) | null/DLC(+0x18->+0x20 via 0x17c3a40[])/bit at profile+0x1290->+0x1410, req loop via 7bb5b0 | verified | f_33b574 |
| f_38ba84 | 0x7bdc70 | unlock mgr: any type-6/7 unlock for item available | same loop: entry +8==id, (type&~1)==6, bit +0x1544, req loop 7bb5b0 | verified | f_338ab8 |
| f_38be9c | 0x7bd6f0 | unlock mgr: CanPurchase/available(user,a,b) -> bool | same flag test (0x122d654/0x122da4d->0x17d5c60/0x17d6065), bit +0x13c4->+0x1544, req loop; used in loop of LIST_PRIMARY_WEAPON_MODS both builds | verified | f_33b574 f_331e6c f_33a3b8 |
| f_38c9f0 | 0x7bda50 | unlock mgr: mark purchased/seen (sets bit, notifies) | sets bit +0x13c4->+0x1544 then calls f_38c280=0x7bd0b0; last call of weapon-mods setup in both builds (pairs.json swapped 38be9c/38c9f0) | verified | f_33b574 f_331e6c f_33a3b8 |
| f_38cde0 | 0x7bca00 | unlock mgr: purchase status 0..4 (0 owned,1 buyable,2 too poor,3 locked,4 DLC) | IsUnlocked->req loop->cost vs money (+0x74c), 38b934/29c510 path returns 3/4; same returns | verified | f_33b574 |
| f_38d17c | unknown | return NP handle *(0x125f2ec) | 24-byte accessor; only callers stubbed | unknown | f_34634c |
| f_38d584 | unknown | voice chat: is talking (SDK call d80374) | PS3 SDK | unknown | f_34634c |
| f_38f2d8 | unknown | find voice/NP slot by id (0x125f718, stride 0x1d0) | not located | unknown | f_34634c |
| f_38f7e4 | unknown | voice slot state (SDK d81154) | not located | unknown | f_34634c |
| f_6cd9d8 | inline | mod bit -> name text-id | PS4 0x7680f0 has the same bit->id if-chain (0x9fe22c98,...0x815211c8) | verified | f_33a3b8 |
| f_6cdae0 | inline | mod bit -> description text-id | PS4 0x7680f0 if-chain (0xac24a8,...0xefad05da) | verified | f_33a3b8 |
| f_6cdd7c | inline | mod bit -> icon id via symbol 0x36a4c63b | PS4: 0x11f2bf0(0x36a4c63b) loop "1<<i==bit" (0x7680f0) | verified | f_338ab8 f_33a3b8 |
| f_6ce8b4 | inline | weapon def lookup in hashed symbol 0x61d589d9 | PS4: 0x11f2e30(0x61d589d9) + modulo probe (0x767a60, 0x5b3e40, 0xd49270) | verified | f_338ab8 f_33a3b8 |
| f_796d78 | 0xf32080 | format number with thousands separator | same /1000 splits, text 0x43511ad3, 0x4fc6dc93 | verified | f_33b574 f_331e6c f_33a3b8 f_346e4c |
| f_799f8c | inline (core 0xf32ae0) | localized text lookup with fallback | PS4: *(u8*)(*(0x1b24b80)+0x4c7c) ? 0xf38f40(id,1)/"(sid #x%08x)" : (0xf32ae0(&0x17026b8,id) ?: *(0x17026c8)); 0xf32ae0 == f_799ed8 | verified | f_33b574 f_331e6c f_338ab8 f_33a3b8 f_340bd0 f_346e4c f_29366c |
| f_79a604 | 0xf32e00 | text with [A]/[B] numeric replacement | same structure, [A]/[B] strings | verified | f_331e6c f_346e4c |
| f_79aafc | none (stripped) | text with [A],[B],[TEXT],[TEXT2],[TEXT3] | "[TEXT2]/[TEXT3]" only referenced by 0xf32bd0 (=f_79a9a8); emulate with 0xf32e00 + manual [TEXT] replace | likely | f_33b574 |
| f_919834 | unknown | friend list: find by name hash (0x135f528) | no PS4 equivalent located | unknown | f_34634c |
| f_91a2d4 | none | Facebook sign-in state | "Facebook" strings absent from PS4 binary | likely | f_34634c |
| f_920ec8 | none | avatar icon loader ("Need to retry %s Load Icon") | strings absent from PS4 binary | likely | f_34634c |
| f_97202c | inline | UI icon lookup | given (loop *(*(0x1b24bc8)+0x28 / +0x20), 40-byte entries) | verified | f_33b574 f_331e6c f_338ab8 f_33a3b8 f_340bd0 f_346e4c f_34634c |
| f_9a77a4 | 0x11f2bf0 | symbol lookup (ptr-to-ptr) | given anchor | verified | f_338ab8 f_33a3b8 f_340bd0 f_29366c |
| f_d64d24 | 0x139cab0 | snprintf | same call sites (e.g. "(sid #x%08x)") | verified | f_34634c |
| f_d804f4 | none | PS3 SDK import stub (OPD 0x111b94c) | PS3-only SDK | likely | f_34634c |

### Group B: f_340bd0 (treasures), f_346e4c (missions)

| PS3 | PS4 | what it does | evidence | conf | called by |
|---|---|---|---|---|---|
| f_5ba34 | 0x662760 | player level(user): data+0x500 -> rank table 0xc85e199d, capped 50 for demo title IDs | reads obj+0x748, symbol 0xc85e199d (+0x68/+0x70), BCET70043/NPUA70180/NPHA80158 checks (=f_44718 inline); DAT_01230770->0x17d7abc | verified | f_346e4c |
| f_9bf04 | inline | treasure owned (f_356470) | callers align to inline symbol lookups | likely | f_340bd0 |
| f_9ccc4 | unknown | treasure set progress (symbols 0xb114fe00/0xf1f296cb) | only caller f_340bd0 (stub) | unknown | f_340bd0 |
| f_2fa940 | unknown | find map entry by id (symbol 0xdec97a4) | not located (likely inlined like 0x7329a0 loop) | unknown | f_340bd0 |
| f_2fad90 | inline | get map/playlist entry i (symbol 0xf985c7b6) or zeroed default | callers align to 0x11f2bf0 lookups; no standalone fn | likely | f_340bd0 |
| f_32c624 | unknown | build treasure list by set (symbol 0xb114fe00) | not located | unknown | f_340bd0 |
| f_32c734 | unknown | build treasure set list (symbol 0xf1f296cb) | not located | unknown | f_340bd0 |
| f_341cbc | inline | get selected item for top page (symbol 0xf1514340) | no standalone fn; callers align to 0x11f2bf0 lookup | likely | f_346e4c f_283828 |
| f_349fa4 | inline | active mission id = data+0x161c | PS4 obj+0x1864 | likely | f_346e4c |
| f_34a018 | inline | mission completed bit data+0xc84 | PS4 obj+0xecc bit test (seen 0x7716c0) | verified | f_346e4c |
| f_34a8bc | unknown | active mission time remaining (SDK time fns) | not located | unknown | f_346e4c |
| f_34ad64 | unknown | active mission progress | not located (likely inlined, cf. 0x772d90) | unknown | f_346e4c |
| f_34ae64 | inline | mission def i (static table stride 0x18) | tiny accessor | likely | f_346e4c |
| f_34d138 | unknown | mission (cur def,tier) not active -> f_34bf40 | pairs.json says 0x772950 (unchecked) | unknown | f_346e4c |
| f_34d5f4 | unknown | is selected mission the active one | not located | unknown | f_346e4c |
| f_38b284 | none found | unlock mgr: find entry whose req list contains (type 4,id) | no PS4 fn with 2-arg (mgr,id) req-type-4 scan in 0x7bb3b0..0x7befc0; only caller f_340bd0 is stubbed | unknown | f_340bd0 |
| f_41233c | unknown | character/skin unlock info (symbol 0xbd3bb3b9) | not located | unknown | f_340bd0 |
| f_79a748 | 0xf33010 | text with [TIME] replacement | refs "[TIME]"; same slot in f_34c45c/0x773b50 | verified | f_346e4c |
| f_79a9a8 | 0xf32bd0 | (callee) text with [TEXT]/[TEXT2]/[TEXT3] | refs those tags; same call in f_796d78/0xf32080 | verified | f_340bd0 |
| f_d67fdc | 0x139c5f0 | memset | PS4 memset(ptr,0,n) pattern used everywhere | likely | f_340bd0 |

### Group C: page/button handlers f_28d358, f_29366c, f_285bc4, f_283828, f_292e3c

| PS3 | PS4 | what it does | evidence | conf | called by |
|---|---|---|---|---|---|
| f_44718 | inline | title-id check (BCET70043/NPUA70180/NPHA80158, 0xc0e2aff8/0x2c3d7d0f) | PS4 inline strcmp(&0x17b9ca0,...) and 0x17b9d10 compares in 0x7270f0/0x662760 (PS3 0x11d1118+0x2208 -> 0x17b9ca0) | verified | f_28d358 |
| f_5af2c | unknown | add money to user (clamped by symbol 0xa87da3d1), data+0x504 | only caller f_29366c is stubbed; not located | unknown | f_29366c |
| f_5c0e4 | unknown | check level/rank requirement (symbol 0xc85e199d, data+0x4fc) | only caller f_292e3c (stub); not located | unknown | f_292e3c |
| f_26056c | inline | current popup page id | PS4: p=*(menu+0x898); p ? *(u32*)(p+0x10) : 0 | likely | f_28d358 f_29366c |
| f_262418 | 0x6530e0 | rebuild page breadcrumb list | same loop; +0x720->+0xdb0, +0x510[]->+0x8b0[], +0x868->+0xef8 | verified | f_28d358 f_292e3c |
| f_262d48 | unknown | queue ticker/notification message (menu+0x1614) | not located | unknown | f_29366c |
| f_264f60 | inline | close popup page (sound 0xd, vcall, f_264d8c) | PS4 inline 0x651fa0(0xd) + 0x6531f0 sequence (in 0x66e400/0x7590a0) | likely | f_28d358 |
| f_265030 | 0x65a000 | open popup page by id | page array +0x2f0(0x84) -> +0x390(0xa0), cur +0x504->+0x898 | verified | f_29366c |
| f_26a08c | unknown | show text-entry/keyboard popup (0x453470dd) | only caller f_292e3c (stub); not located | unknown | f_292e3c |
| f_26a238 | 0x652310 | ShowWaitPopup (handler 0x471007a1) | "ShowWaitPopup" assert; +0x1364/+0x1368 -> +0x2160/+0x2168 | verified | f_28d358 f_29366c |
| f_26ad98 | 0x652200 | menu back / re-show previous page | same logic, f_269b28 inlined | verified | f_28d358 |
| f_2787bc | inline | copy player-card struct to profile + save | callers align to inline code + 0x7be1d0 | likely | f_29366c |
| f_278ca4 | unknown | player card render-target setup | not located | unknown | f_28d358 f_29366c |
| f_2793dc | unknown | reset player card struct | not located | unknown | f_28d358 f_29366c |
| f_27e1c0 | inline | f_2f6304 wrapper (+7 byte) | tiny | likely | f_29366c |
| f_27f268 | unknown | per-frame MP menu update ("SFX: Countdown") | not located (0x5b79e0 is NOT it) | unknown | f_285bc4 f_283828 f_292e3c |
| f_28cb80 | unknown | enter/leave MP menu (waits, inits) | not located | unknown | f_28d358 f_29366c |
| f_292e3c | RET stub 0x66edf0 | (page handler, itself a target) | seed pair; PS4 is ret | verified | f_29366c |
| f_29c500 | inline | DLC owned byte: *(dlcmgr+0x110+i) | PS4 (&0x17c3a40)[i] (0x121b6e8 -> 0x17c3930) | verified | f_28d358 f_29366c |
| f_29c738 | inline | owned DLC map bitmask (0x121b6e8 +0x174/+0x18c) | callers align to symbol lookups | likely | f_292e3c |
| f_2b5d14 | unknown | find language/region entry (symbol 0x75e3b9a9) | pairs.json says 0x6a8010 (unchecked) | unknown | f_28d358 |
| f_2f6304 | unknown | local NP player record (+0x1dc if +0x25c==0x60) | not located | unknown | f_29366c |
| f_2f7e70 | unknown | find friend by name | not located | unknown | f_29366c |
| f_2f7f10 | none | Facebook name fetch ("Get Facebook name") | string absent on PS4 | likely | f_29366c |
| f_2f9800 | unknown | set flag 0x12b4c14 + f_79f310 | not located | unknown | f_28d358 |
| f_2fafe0 | 0x73f7d0 | find playlist by id (symbol 0xc1b0be0d), DLC-filtered | entry stride 0x5c->0x68, DLC byte array 0x17c3a40 | verified | f_292e3c |
| f_2fb210 | inline | selected playlist id = data+0x52c or 0xb70ed28 | no standalone fn found | likely | f_292e3c |
| f_2fedc4 | 0x73f990 | n-th available playlist | same counting + DLC filter (PS4 drops 0x67 party check) | likely | f_292e3c |
| f_300930 | unknown | mode index by id (symbol 0x3b9a067d) | only caller f_29366c (stub) | unknown | f_29366c |
| f_300a40 | 0x721050 | current game-mode entry ptr | same | verified | f_29366c |
| f_303f6c | unknown | playlist +0x44 for selected playlist | only caller f_292e3c (stub) + f_341e4c (inlined there?) | unknown | f_292e3c |
| f_304274 | unknown | playlist +0x1c for selected playlist | only caller f_292e3c (stub) | unknown | f_292e3c |
| f_30573c | 0x720610 | net/playlist helper | given anchor (0x720610 calls 0x73f7d0 = f_2fafe0, consistent) | verified | f_29366c |
| f_3077b0 | unknown | team count/size for mode (2-arg switch) | not located (likely inlined); only caller f_292e3c (stub) | unknown | f_292e3c |
| f_307a3c | 0x7329a0 | map entry for current mode (symbol 0xdec97a4) | stride 0x54->0x98 | verified | f_292e3c |
| f_307f24 | 0x73f5c0 | max players for mode | PS4 uses table 0x13a61e0 = {10,6,10,10,3,...,8(0xe),10,1(0x10),3,4(0x12)} == PS3 switch | verified | f_292e3c |
| f_31c304 | inline | set *(0x122d650+0x3c) | setter; no standalone fn | likely | f_292e3c |
| f_31c374 | inline | *(0x122d650+0x24) (online-ready flag) | PS4 tests DAT_017d5c80 in the same place (f_355934 inline) | verified | f_28d358 f_29366c |
| f_321e04 | unknown | build local player info block (DLC bits, levels) | not located | unknown | f_29366c |
| f_32c02c | unknown | format time/date string into 0x1232980 | not located | unknown | f_29366c |
| f_34d854 | none | send network message (SDK d812b4, "Send Message %x") | string absent on PS4 | likely | f_29366c |
| f_35314c | unknown | tiny accessor | not investigated | unknown | f_29366c |
| f_353334 | unknown | party/session helper | not investigated | unknown | f_29366c f_292e3c |
| f_355934 | inline | get profile data ptr (screen obj+0xc8), lazy default-init | PS4 inline: obj=*(0x17dd610)+u*0x7b00; if *(obj+0x7ae8)==3&&!0x17d5c61&&0x17d5c80 -> init; data=obj+0x248 | verified | f_28d358 f_29366c f_285bc4 f_292e3c |
| f_355d24 | unknown | profile helper (0x1248640) | not investigated | unknown | f_283828 |
| f_356198 | 0x77fb80 (pairs.json, unchecked) | apply loadout to player | not verified | unknown | f_28d358 |
| f_3582f4 | unknown | profile helper | not investigated | unknown | f_29366c |
| f_35847c | none found | NetPlayerData publish thread (save profile) | thread name string absent on PS4 | unknown | f_28d358 f_29366c |
| f_373f8c | unknown | party helper | pairs.json says 0x73f7d0 (wrong: that is f_2fafe0) | unknown | f_292e3c |
| f_37410c | unknown | party accessor | not investigated | unknown | f_292e3c |
| f_374118 | unknown | party reset | not investigated | unknown | f_292e3c |
| f_374318 | unknown | party/session state accessor | not investigated (likely inline) | unknown | f_29366c f_292e3c |
| f_3744c8 | unknown | party helper | not investigated | unknown | f_29366c f_292e3c |
| f_3755d0 | unknown | party helper | not investigated | unknown | f_292e3c |
| f_37640c | unknown | party flags | not investigated | unknown | f_292e3c |
| f_384118 | none found | "Select Second User" flow | string absent on PS4 | unknown | f_29366c |
| f_38c280 | 0x7bd0b0 | unlock mgr: on-unlock notify/refresh (called after setting purchased bit) | positional: last call of f_38c9f0 / 0x7bda50 with (mgr,user) | verified | f_28d358 |
| f_38e4f4 | unknown | script/DC event (f_26a868) | not investigated | unknown | f_292e3c |
| f_4109d4 | unknown | player model/skin setup (symbol 0xbd3bb3b9) | pairs.json says 0x9954f0 (unchecked) | unknown | f_28d358 |
| f_41206c | unknown | f_411ca8 + flag | not investigated | unknown | f_28d358 |
| f_70af24 | likely one of 0xd98c80..0xd98e10 | RNG (multiply-with-carry 0x7dd4ffc7) -> double | same constant; RNG also inlined in ~190 PS4 fns | unknown | f_29366c |
| f_884fe4 | none | PS3 SDK wrapper (d80214) | SDK | likely | f_285bc4 f_283828 |
| f_8851bc | none | PS3 SDK/heap helper | SDK | likely | f_285bc4 f_283828 |
| f_89f0f0 | unknown | engine (pairs.json 0x11bb590, unchecked) | not verified | unknown | f_28d358 |
| f_8a0118 | unknown | engine bitset search | not investigated | unknown | f_28d358 |
| f_8a1894 | unknown | engine string-id init | not investigated | unknown | f_28d358 |
| f_8a487c | unknown | engine accessor (pairs.json 0x139dbe0, unchecked) | not verified | unknown | f_28d358 |
| f_9333c0 | unknown | timer expired (0x12f1f38+0xf8) | tiny, likely inline | unknown | f_29366c |
| f_933490 | unknown | timer set | tiny, likely inline | unknown | f_29366c |
| f_958b9c | unknown | engine list find | not investigated | unknown | f_28d358 |
| f_95a3ec | unknown | engine wrapper | not investigated | unknown | f_28d358 |
| f_95d044 | unknown | engine map contains | not investigated | unknown | f_28d358 |
| f_9a77e0 | 0x11f2e30 | LookupSymbolValue | given anchor + same assert string | verified | f_292e3c |
| f_9e5f94 | unknown | engine map lookup (hash 0x6e51b48f) | pairs.json says 0x125fdb0 (unchecked) | unknown | f_292e3c |
| f_9e6c44 | unknown | engine lookup | not investigated | unknown | f_292e3c |
| f_d5c5dc | libc memcpy (0x139c5e0?) | memcpy | PS3 libc; PS4 libc thunks around 0x139c5e0 | unknown | f_285bc4 f_283828 |
| f_d5ee24 | libc (unknown) | libc helper | PS3 libc | unknown | f_292e3c |
| f_d64180 | 0x139c710 | strcmp | same call sites (title-id strcmp, party name compare) | verified | f_29366c |
| f_d6498c | libc (unknown) | libc helper (strtok/strstr?) | PS3 libc | unknown | f_29366c |
| f_d80cd4 | none | PS3 SDK import stub | PS3-only SDK | likely | f_29366c |
| f_d80e14 | none | PS3 SDK import stub | PS3-only SDK | likely | f_292e3c |
| f_d81074 | none | PS3 SDK import stub | PS3-only SDK | likely | f_29366c |
| f_d81294 | none | PS3 SDK import stub | PS3-only SDK | likely | f_28d358 |
| f_d82374 | none | PS3 SDK import stub | PS3-only SDK | likely | f_285bc4 f_283828 |