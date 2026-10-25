# Local session reconstruction

Scope: PS4 1.00 reconstructed ELF with SHA-256
`31129a2fee4a6442dce0cc2c26ced3519763b1d28500ade47acf6a21d968404c`.
This is a backend experiment, not an integrated or playable in-game lobby.

## Preserved operations verified with Rizin 0.9.1

| Address | Observed operation |
|---|---|
| 2f70e0 | Clears ten member-active flags, room pointer, selected pending-operation fields |
| 2f7230 | Returns active member matching 32-bit field at member+28 |
| 2f7270 | Returns active member by index; caller must validate index |
| 2f7290 | Returns active member whose +2c matches session+b04 |
| 2f72e0 | Returns active member whose +2c matches session+b00 |
| 2f7320 / 2f7330 | Set session+b00 / session+b04 |
| 2f74e0 | Sums active bytes if room pointer is non-null; otherwise returns **1** |

Member array starts at session+20c, stride d0, ten entries. Active byte is
member+25. These facts do not establish the complete room/member ABI or the
meaning of every field. The two identity selectors should not be labelled host
and local user until their call sites establish which is which.

The reset is not a constructor: it leaves many fields untouched. Newly allocated
test storage must be zeroed. Derived-session reset at 2edfc0 calls 2f70e0 then
clears b50, b54, b58, b60 and b88. An arbitrary live object cannot be reset safely
without checking ownership and lifecycle.

Evidence: `work/rizin-mp/local-session-leaves.txt`, with loader warnings retained
separately. Rizin code-byte observations are separate from any execution tests.

Implementation: `tools/uc2mp_local_session.py`. Run in a separate Windows x64
Python process with the verified ELF path. Eight original leaf functions are
executed, while Python supplies the isolated member records and opaque token.
`tools/test_uc2mp_local_session.py` passed three tests covering reset/recreate,
native pointer results, selectors, capacity, input bounds and ELF rejection.
The CLI also returned slot zero for both selectors and the expected member ID.
See MULTIPLAYER-LOCAL-BACKEND.md for code-copy and ABI validation details.

## Integration boundary

The party-mode call site at 2bcaa1 passes global e73178 to member count 2f74e0.
Another session at e719d0 is passed to index lookups around 2de5a3. These are
different objects; do not substitute one merely because the base layout matches.

The original creation APIs are not usable as-is: 2f7420 returns zero without
creating anything, and 2f75e0 asserts false in SetData. The menu entry callback
2bb1a0 is also empty. Success-like return values do not establish readiness.

A standalone local adapter may own zeroed session storage and an opaque room
token solely for these verified leaf queries. Such a token is **not** a valid
guest matchmaking room and must never be installed into the running game.
Names retained by the adapter are host metadata, not a recovered PS4 name field.

## Remaining work before an in-game lobby

1. Recover the room/member layout consumed by the party display and its update
   callbacks; distinguish party e73178 from game session e719d0.
2. Implement missing create/close operations with real owned objects, completion
   events and cleanup; map display-name fields instead of guessing offsets.
3. Connect the BUTTON_MULTIPLAYER callback to the new lifecycle and page transition
   only after those contracts are tested. Keep normal launch unchanged.
4. Verify the local player's visible row, selection controls, exit and re-entry
   in the game. Packet transport and two-process gameplay are later milestones.
