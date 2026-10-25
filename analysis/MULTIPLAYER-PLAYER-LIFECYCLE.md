# PS4 1.00 player/profile lifecycle findings

Static evidence for SHA-256 31129a2fee4a6442dce0cc2c26ced3519763b1d28500ade47acf6a21d968404c. No live calls, patches or injected writes performed in this investigation.

## Preserved real constructors and ownership

- `2efd30(player*)` initializes a 0x2c0-byte player record. It zeroes owned range fields +168/+170 before the reset-like work, constructs the subobject at +1b0 via `1fe5a0`, then fills defaults. Uses aligned `vmovaps [player]`, so player alignment must be 16 bytes.
- `2efff0(player*)` resets an ALREADY initialized record; it reads +168/+170 and calls `3cfc00`. Do not execute against uninitialized arbitrary memory.
- `2d2f10(manager*)` constructs ten players at +10+n*2c0 via2efd30. Clears local/special pointers at+0/+8 and all pool fields. This is constructor-like and distinct from initialization.
- `2d3160(manager*)` resets players then allocates two pools through `4e5690(size,alignment=16,...)`: size0x6f0 stored+1ba0, size0x4048 stored+1c10. Populates lists/locks, configures game mode via2d37b0(manager,0,NULL,10), and loads game data. Existing pool pointers are overwritten without a preceding free in this routine; do not repeatedly call it to refresh a roster. Constructor/initializer require the real game registry, allocator and imported guard state.
- `2d3710(manager*)` only resets player slots and clears manager+0/+8. It is not equivalent to full initializer or pool teardown.

## 2d37b0 is configuration, not player insertion

Full function 2d37b0..2d39bb inspected. Arguments RDI manager, ESI configuration index (stored as word+1ca0), RDX optional 20-byte configuration, ECX mode. Registry40d3b0(3b9a067d) supplies 0x90-byte mode records. It writes manager+21b4..21c4, calls rank getter2eef10(profile f44840), and loads related data through registry0dec97a4. It never inserts/activates a player.

## Real player layout established by consumers

| Relative player field | Evidence |
|---|---|
| +20 active | 2f03e0 returns active && !flag26c; manager iteration2d5c10 reads manager+30+n*2c0 |
| +120 numeric identity | 2d8220 scans manager+130+n*2c0, returns index; 2d82c0 returns player pointer |
| +240 string/name candidate | 2d8260 compares input string against manager+250+n*2c0 using strcmp b0f540 |
| +26c exclusion flag | 2f03e0 |
| +168 pointer/+170 range | read by reset before3cfc00; constructor zeroes them |

Manager+0 and+8 are distinguished player pointers, read by2d2d50 and2d8300. Their exact local/host roles still require UI usage evidence. `2d3a40(manager,index)` bounds index<=9 then returns slot regardless of active flag. `2d8220` and2d82c0 do not check active either; don't assume a zero identity finds no player. `2d8260` also scans all ten names regardless of active flag.

## Profile f44840

`2ee940(profile*)` returns profile+44 but is NOT a pure accessor. If profile+6c0 is3 and both2bb090()/301720() return false, it writes format/version10 at+44, zeros0x224 bytes at+48, sets+6c0 to2. Calling it can reset stats. `2ee670(stats*)` initializes version10 plus0x224 zero bytes; this is a stats payload initializer, not a player or identity constructor. Several preceding profile operations2ee5e0/5f0/600/610 are immediate returns.

## Missing insertion remains explicit

No real local-player insertion/identity activation was identified in these bounded ranges. A linear Capstone scan of2b8000..310fff found no nonzero direct store to player+120 nor active byte+20=1 in the player-manager routines. This does not prove absence: memcpy, alternate address arithmetic or other code may populate records. Do not claim manual field initialization is an original creation function.

Evidence under work/rizin-mp: player-profile-contracts.asm (Rizin), player-profile-capstone.asm, player-constructors-capstone.asm, player-slot-capstone.asm, player-activation-candidates.asm. The earlier ps4-game-manager-capstone.asm has complete2d37b0 configuration disassembly.
