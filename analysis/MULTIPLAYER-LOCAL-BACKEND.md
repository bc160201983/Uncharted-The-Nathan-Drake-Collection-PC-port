# Isolated native-session milestone

`tools/uc2mp_local_session.py` executes eight original PS4 1.00 leaf routines in
its own process. It never opens the running game or writes game files. This is
backend execution evidence, not an implemented in-game lobby.

The accepted ELF SHA-256 is
`31129a2fee4a6442dce0cc2c26ced3519763b1d28500ade47acf6a21d968404c`, checked before
allocating native code. PT_LOAD mapping uses `uc2mp_entry_audit.file_offset`.
Only the exact whitelist is copied into RW memory, then changed to RX. Windows
to SysV thunks preserve RDI/RSI and align the stack. No import binding, live
injection, PSN impersonation, or transport occurs.

## Verified preserved behavior

- `2f70e0`: reset clears ten active flags and room pointer; initializes async
  status bytes. It is not a complete constructor for derived session classes.
- `2f7230`: finds active member by record+0x28.
- `2f7270`: gets active member at index; caller must bound index 0..9.
- `2f7290`, `2f72e0`: find members whose record+0x2c matches session+0xb04 or
  +0xb00. Named selector B/A here until their host/local meanings are proven.
- `2f7320`, `2f7330`: set those selectors.
- `2f74e0`: sums active flags if room pointer is nonzero, otherwise returns 1.
  Closed snapshot consequently distinguishes logical_count=0 from native_count=1.

Backend test records occupy session+0x20c+n*0xd0, active byte +0x25,
lookup ID +0x28, identity +0x2c. Python supplies the record data; no original
create-member implementation has been established. The nonzero room token is
opaque allocated test storage, used only as a boolean by these exact leaves.
It must never be passed to other engine/session functions.

## Bounded findings that prevent an in-game claim

`2f7410`, `2f7480`, `2f7560`, `2f7590`, `2f75d0` are immediate-return stubs.
`2f75e0` identifies itself as net-session.cpp SetData and explicitly asserts
false, then executes int 0x41. No working create-local-member routine was
identified in this inspected neighborhood.

`2d3710` resets ten player records at object+0x10+n*0x2c0 via `2efff0` and clears
object+0/+8. `2efff0` reads existing allocation fields +0x168/+0x170 and calls
`3cfc00`; it is not safe to treat it as a dependency-free constructor. Neither
routine is executed by this backend.

Evidence: `work/rizin-mp/ps4-local-session.asm`,
`ps4-session-constructors.asm`, `ps4-local-more.asm`,
`ps4-player-setup.asm`, `ps4-game-manager-capstone.asm`. Some initial bounded
Rizin output hits its block boundary and shows invalid decoding; exact whitelist
bytes were additionally decoded completely with Capstone: all eight ranges
decode without gaps, all branches remain within their range, none calls another
function or uses RIP-relative memory, and none writes RBX/RBP/R12-R15. RDI/RSI
are preserved by the adapter. They were executed only after the whole-image
identity check. The tests exercise every whitelisted routine.

## Validation

Run as a subprocess: `python tools/test_uc2mp_local_session.py`.
Tests cover lifecycle/repeated close/recreate, original getter pointer results,
independent selectors, capacity ten, index rejection before entering unbounded
native indexing, duplicate IDs, uint32 boundaries, and unknown-image rejection.

Example: `python tools/uc2mp_local_session.py work/base-1.00/eboot.elf`.
To achieve the requested visible lobby, the next integration must identify the
active party session and its derived initialization, initialize the real local
player/profile record, and connect the native menu update/dispatch lifecycle.
