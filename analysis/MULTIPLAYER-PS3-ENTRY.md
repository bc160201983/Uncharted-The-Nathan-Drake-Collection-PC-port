# PS3 multiplayer entry contract (static evidence, 2026-10-24)

Reference: `<ps3recomp>/UC2MP/game/BCUS98123/EBOOT.ELF`.
SHA-256: `7ad3b270aac6ec920064eccc12d1a35ebb4b99806ca22b148367e71f8ddb5e09`.
Extracted only `menu.bin` from `PS3_GAME/USRDIR/build/main/dc1.psarc`, using the PSARC block/manifest layout already implemented in `tools/install_update.py`. No source game changes.
Menu size 58,836 bytes, SHA-256 `c6e57e15fd891cc19c68458490dd16a48d74c0e86624b4d04736279cf63b8c88`.

## The original button is a callback, not a direct page link

PS3 records at menu offsets `0x2800`, `0x28dc`, `0x2adc`: name `BUTTON_MULTIPLAYER`, ID `0xed9d8ae1`, label `MENU_MULTIPLAYER` / `0xf75b3a65`. The paired action/condition fields at +0x1c/+0x20 and page pointer/ID at +0x30/+0x34 are all zero. Contrast `BUTTON_MULTIMEDALS` at 0x2890/0x29b4: action 0x13, condition 8, target PAGE_MULTIMEDALS / 0xace1a9d5. PAGE_NEWPROFILE's array starts at 0x2770 with five 0x48-byte entries; multiplayer is its third entry.

PS3 registration 0x439394..0x4393ac supplies ID ed9d8ae1, OPD deed80, context 0 to 0x422344. This registration function appends {ID, callback, context}, stride 12, at menu manager+0xaa0; count at +0xa9c. TOC e18380 -> [TOC-78dc]=db8290, [db8290-7ef8]=deed80; OPD resolves handler 0x43b320.

Handler 0x43b320 logs `Multiplayer`, calls 0x935c8(global e61c1c), returns if nonzero. Otherwise calls 0x438d98([e61ed4+0x2315]); if true it returns, if false it tailcalls 0x4388a0. The latter is an abort/cleanup sequence, not the bootstrap: it tears down several network objects and resets state. These semantics are inferred from control/data flow; original symbols are absent.

## Bootstrap 0x438d98

- Calls 0x911110 with string `multi-player`, sets [f11bf8]=1, calls 0x4c3964, clears [f1fa48+0x2b0].
- Calls imported cellNetCtl stub ae0f2c (NID 8b3eba69) and requires returned state=3. Failure opens a message then returns false.
- Calls 0x4ec138 and NP stub ae1fec (NID 6ee62ed2), checks returned restriction values. Calls 0x4aa968(4). These are prerequisites; blindly reporting success cannot supply the later objects.
- Computes a content-version value, calls 0x4aac6c(f1fa48,value), emits `Game Data Version = %x`.
- Sets a transition via 0x429bd8(menu f0e800, ID 9e1863b1, 0).
- Calls sysPrxForUser stub ae346c (NID 24a1ea07): thread entry OPD def0e0 -> code 0x447c28, argument 0, priority 2, stack size 0xc000, flags 0, name `NetInit`.
- Calls 0x76bc28(global,1) and returns true. NetInit is asynchronous; this return is not session readiness.

## NetInit thread 0x447c28..0x4497c4

Early phase logs `NetInit Started`, invokes sceNpUtil bandwidth-related calls, gets NP ID through ae252c (NID fe37a7f4), copies it into f1fa48+0x288, registers a manager callback through ae240c, requests a ticket via ae20ac, polls/retrieves it through ae1d2c and ae1f2c. Logging strings independently identify these calls. It selects service/content URLs, attempts live-data and patch processing. This historical PSN/CDN sequence should be mapped, not replayed as an assumed working PC service.

Later, explicit object initialization sequence:

| Call site | Target and arguments | Evidence |
|---|---|---|
| 448f54 | ae22ac(title object, OPD [pool+87a4]=deeca8, 0) | failure text `sceNpBasicRegisterHandler() failed` |
| 448f70 | 4d91e8(f11978) | failure text `g_sessionManager.Init()() failed` |
| 448f88 | 4d8060(f11978) | result checked against configured requirement |
| 448fe0 | 4d0d90(f20c48) | object initialization, exact class not yet proven |
| 448ff0 | 4c4a70(f2c8a8) | object initialization, exact class not yet proven |
| 449000 | 4d83d0(f11978,f20c48) | attaches first object to session manager |
| 449010 | 4d83d0(f11978,f2c8a8) | attaches second object |
| 449020 | 4d42a0(f2c8a8,1) | mode/enable call; semantics need inspection |
| 44907c | 4d80b0(f11978, chosen configured value) | selects configured session/server data |
| 449090 | 4d0e2c(f2c8a8, selected->word0) | applies selected value |
| 44909c | 4dc74c(f10e18) | failure text `g_udpSocket.Init() failed` |
| 4490d0 | 4dc5a8(f11270,3232) | failure text `g_udpSocketWorld.Init() failed` |
| 4490fc | 4e9df8(f3a068) | failure text `g_npConnectionManager.Init() failed` |

Then netctl/NAT queries, menu setup calls, content/audio/DLC preparation, `NetInit Done`, and [f10e14]=1. A functioning lobby requires this asynchronous sequence or a reconstructed local equivalent, not only a page switch.

## Evidence files and limits

`work/rizin-mp/ps3-menu.bin`: extracted resource.
`ps3-button-id-refs.txt`, `ps3-button-dispatch.txt`, `ps3-mp-entry-initialization.txt`, `ps3-netinit.txt`: bounded excerpts from the existing EBOOT disassembly, with function boundaries from EBOOT.functions.json.
`ps3-netinit-calls.txt`: calls annotated with EBOOT.imports.json and printable constants resolved through the ELF LOAD segments.
`ps3-entry-rizin.txt`: fresh Rizin 0.9.1 disassembly independently confirming callback and selected bootstrap/NetInit instructions. `ps3-entry-rizin-warnings.txt` preserves ELF loader warnings.

Static PS3 evidence only. No game execution, PSN communication, hook deployment, or validation of a PC object layout was performed. PS3 addresses/32-bit pointers cannot be copied into PS4 x64 calls. Main investigation must map surviving PC functions, verify constructor/layout/update dependencies, then implement and test a local session lifecycle before claiming multiplayer works.
