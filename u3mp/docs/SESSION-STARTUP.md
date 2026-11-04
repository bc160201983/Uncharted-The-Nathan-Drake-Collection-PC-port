# Session startup work and preserved menus

The menu visibility and session implementation are separate milestones. The remaster retains native page definitions for the Multiplayer choice page, online lobby shell, Friends, and Choose LAN Profile. Native code behind several of these pages is stripped. A restored page is not evidence of a working session.

**Preserved manual build:** `outputs/builds/menu-shell-v1/boot-linked.bin`, SHA256 `8979cc0d2e0590d61bdf7935ab8c29e617010aa2aa096584cd868d004058554b`. Both CMD launchers use this frozen image and verify its digest; they do not rebuild from mutable helpers. The 85-second run `menu-flow-1792939177305836700` remained alive until its limit. Neutral replay isolated physical input, then staged file pulses after observing the profile screen reached and captured both choice and lobby pages. Those later staged pulses are documented here; they are not in the initial result.json input plan. Captures: `evidence/preserved-choice-verified.png`, `preserved-lobby-verified.png`. Earlier purely timed attempts navigated before the profile menu became ready and are not proof of a menu regression.

The failed state-mask experiment in `menu-flow-1792938730320837200` treated states 0/5/6 as the only interactive states, incorrectly blocking the Multiplayer callback. That mask describes reset/hidden states in the inspected callers. The corrected helper rejects only the page-transition states 7/8 that ShowPage itself writes. The manual launcher must preserve both the choice page and online lobby shell.

Routing now uses the actual page stack: normal profile -> PAGE_MULTIPLAYER_CHOICE (0x1e66a744); Play Online -> PAGE_MULTIPLAYER (0x53cbb5f2), loading native net data first; LAN Party -> PAGE_LAN_CHOOSE_PROFILE (0x4608c1ad). The LAN target follows PS3 callback 0x27fdf0. Native Back behavior is retained. The player-action callback 0x671d00 now declines actions while the session backend is absent: otherwise an empty lobby could dereference a null player identity at 0x671de1. No backend-success value or fake member is supplied.

Fresh analysis establishes additional missing work:

- Original PS3 online callback 0x290bd8 calls NetInit 0x2901e0. LAN profile page event 0x290a04 eventually calls NetInit(0,1).
- PS4 LAN profile event 0x673450 and profile selection 0x672c60 are RET stubs. The page can display Create New Profile, but its action is not yet reconstructed.
- PS4 lobby page event 0x66e560 and Start Match 0x66edf0 are RET stubs.
- Backend globals 0x17d6428 and 0x17dd768 remain unset. Retained create/search code requires real backend methods/results.
- The sole libSceNet import in the prepared import table resolves to sceNetInetPton; the table does not supply socket/bind/send/recv imports for a retained LAN implementation.

## LAN discovery foundation

PS3 0x3700b0 sends a single C8 byte to broadcast port 9012. Responder 0x370260 emits C9, big-endian u32 room count, and 30 bytes per room: BE u16 TCP port, BE u64 room ID, BE u16 member count, BE u16 capacity, 16 raw host-name bytes. It advertises only eligible bound sessions whose server is in state 1, and replies to the actual sender's UDP address and port.

`u3_lan_discovery.py` implements this codec and a standalone empty-registry responder. The elevated local-only self-test passed: 127.0.0.2 sent C8 to 127.0.0.1:9012 and received C900000000 at its actual source port. Malformed messages were rejected. The one-room fixture is synthetic codec input only and is never advertised. This service is not connected to the remaster; it is a tested protocol component, not a session implementation. The sandboxed first loopback attempt timed out; the authorized local network test then passed.

Evidence: `evidence/ps3-lan-discovery-codec.c`, `.asm` companion, `lan-discovery-loopback-test.json`, `ps4-empty-lobby-selection-fault.c`, and runtime test logs.
