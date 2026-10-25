# Native PC lobby menu bridge

Status: original UI shows Tony under Chef du groupe (renderer capture and user
confirmation, 2026-10-24). Ctrl/back closes the host lobby and re-entry creates
exactly one member, confirmed by runtime logs. A host-side lobby and menu transition are not a
playable multiplayer match. A later LAN transport and custom-settings extension
passes native tests; its new UI controls still require in-game visual validation.
Gameplay synchronization is not implemented.

The native model is `gow3_pc/src/runtime_uc2mp.c`; the guest UI adapter is
`gow3_pc/src/uc2mp_menu_bridge.h`. Enable `UNCHARTED_LOCAL_LOBBY=1` and generate
the corresponding `tools/uc2mp_menu.py --local-lobby` overlay. `UC2MP_PLAYER_NAME`
sets the UTF-8 profile name (default Tony). All host lobby data stays outside
guest player/session objects.

## Evidence and address contract

Addresses are image virtual offsets for the reconstructed PS4 v1.02 ELF with
SHA-256 `09546f611636dd642fceab51469714e1a7ffd529fa3a009525659902882bad18`.
Actual Rizin 0.9.1 output is saved in
`work/rizin-mp/ui-bridge-1.02.asm`. The reconstructed ELF produces loader warnings;
code bytes and relocation entries were also read directly from PT_LOAD/file data.

| Contract | Evidence |
| --- | --- |
| Multiplayer click | Registration at 5ed9db stores control ed9d8ae1 and callback 5eb160. Callback is a 16-byte ret/NOP island. |
| Click event ABI | Dispatcher 5cb8fd..5cb912 writes control ID at event+0, another u32 at +4, context64 at +8; calls with SysV RDI=event. |
| Open page | 5cb990 accepts MenuHost*, page ID in ESI, flags in EDX; returns success in AL. Global host=1136640; page=53cbb5f2. Existing caller 5ed177..5ed188 uses that exact page and flags0. |
| Roster query | Table105e730 has 24-byte records: command16+i, callback64 at +8 to 659bb0, ordinal i at +16. The callback is another 16-byte ret/NOP island. |
| Text ABI | 65f6fa reads command from value+40. 65f727 sets query1. Dispatcher65f778..65f799 writes ordinal at event+48 and calls callback(event). 65f79c reads text pointer64 at event+40. |
| Text virtual slot | ELF RELA target100b458 points to 65f6d0; constructor65f4ac installs vtable100b430. The adapter replaces only that slot and forwards other commands to the unchanged original function. |
| Gate | Original text method returns empty if global14ce97c<=1. Adapter handles only known party commands16..25, avoiding any write to that network state. |
| Leave | Control afb7834c is BUTTON_MULTIPLAYER_LEAVE_MULTIPLAYER; registration5eda5d points to5eb180. Original body loads global1136640 then tailcalls5cc6f0. Back returns success in AL. |

The menu overlay supplies the original multiplayer control ID/action0, rather
than a direct-page shortcut. PAGE_MULTIPLAYER's previously empty +60/+68 pair is
bound to the existing leave control name/hash; the same descriptor fields on
PAGE_MULTIPLAYER_EULA and MOTD already contain that control. Runtime confirmation
of this Back binding succeeded: the close log occurs between two entry logs.

All hooks are installed before executable protection, with segment permissions,
exact original code signatures, relocated vtable pointer and ten table rows
validated before writing. Unknown images fail closed. The query adapter changes
only opcode1 and known ordinals0..9; it leaves all other query fields untouched.
Text strings belong to the persistent host model. No name pointer is passed into
guest player/session methods. Controls outside party commands16..25 retain their
original implementation. Ready, invitations, game modes and match launch remain
unconnected.

## PS3 cross-check

The supplied PS3 executable (hash starting `7ad3b270`) has analogous table e15d30,
whose party commands16..35 reference OPD df1808 -> function4ba4b0. Rizin output:
`work/rizin-mp/ps3-roster-query.asm`; lifted source:
`<ps3recomp>/UC2MP/src/recomp/ppu_recomp_002.cpp`,
`func_004BA4B0` at line169834. Query1 produces the name at PS3 event+30; query8
refreshes a roster list; ordinals are reduced modulo10 but upper ordinals also
alter team/name formatting. The PC adapter deliberately handles only the first
ten until the secondary page contract is reconstructed. Build version should be
identified by hash; the PS3 project notes describe a 1.09 base.

## Runtime validation

Observed in work/local-lobby/analysis102/big2-ps4_Shipping/boot.log: bridge
installed; local lobby opened host=Tony,members=1; original UI requested row0;
leave-multiplayer command closed the lobby; re-entry again reports members=1.
The renderer output_000_1557x876.raw shows Tony below Chef du groupe. User
independently confirmed both name display and working return. C tests pass
50 model lifecycles, profile validation, roster operations/host controls, query
field isolation, empty unused rows and unsupported-image rejection.

Use Lancer-UC2-Salon-local.cmd (tools/uc2mp_local_launcher.py). It copies the
compiled runtime into work/local-lobby/runtime, regenerates the callback menu
overlay, and runs with isolated user data/logs. Normal saves and the installed
runtime remain unchanged. No automatic timeout is applied by this launcher.
The original milestone above describes the user-validated roster-only build.
The current development version extends it as follows (2026-10-24 night):

- Both roster pages now map commands16..35 to native members (ordinal modulo10).
- Command52 supplies the Custom label required by original CanSelect57a780.
- Mode57 and map58 use verified MenuData virtual methods: current65f940,
  set65fa30, min65fb20, max65fc20, item65fd10 and enabled65f5d0. Query5 is MAX,
  query6 is MIN. Other methods still forward to the original implementation.
- Settings are host-owned IDs, never guest manager indices. Host-only changes
  reset readiness; original launch command50 remains unavailable.
- Native UDP LAN supports up to10 members with roster/settings/ready snapshots,
  loss recovery, bounded timeouts and graceful close. Only explicit --host or
  --connect enables it. Token is a shared bearer key in cleartext for trusted LAN.
- The menu-thread bridge opens/polls/closes the transport, using a fresh random
  nonce on every session. Unicode profile names pass via UTF-8 hex environment
  data to avoid Windows code-page corruption.

The transport's real UDP loopback tests pass, including reordered packets,
malformed snapshots, reconnect and leave-before-delayed-join. This does not prove
two graphical game instances display the same group yet. New launches currently
fail before guest initialization in Vulkan surface-capabilities discovery;
this affects the earlier known-good binary as well. The old roster capture is
not evidence that the new dropdowns or network bridge have run visually.

Build targets: gow3-probe, uc2mp-test, uc2mp-menu-test, uc2mp-lan-test and
uc2mp-config-test. Evidence for the additional UI contracts:
work/rizin-mp/ui-custom-settings-1.02.asm. Map contracts and remaining launch
requirements: MULTIPLAYER-MAP-LOADING.md.

## Page status and safe custom-page navigation

Static evidence in `work/rizin-mp/ui-status-start-1.02.asm` verifies Start
ID `70e80076` registration at `5edadf` targets `5eb780`, an immediate RET.
The bridge keeps command50 disabled and now rejects installation unless both
the registration and the entire 16-byte RET/NOP island match. It does not
implement or advertise match launch. PAGE_LOBBY's menu descriptor at `1200`
has zero custom Back binding at offsets60/68, so its generic Back is preserved;
the explicit leave-and-close binding belongs only to PAGE_MULTIPLAYER.

Command98 is the original page title query. Table row `105ec88` contains
command98, callback `65ecc0`, ordinal1. The callback's query1 branch resolves
the title and writes pointer64 at event+40. The linked image relocation record
at file offset `4c0e8` in `work/local-lobby/analysis102/big2-ps4_Shipping/boot-linked.bin`
is `{105ec90, 0, 65ecc0, 0}`. The installer validates this relocated pointer,
the row and callback prologue before enabling a native title/status label.
It shows local/LAN member and ready counts or the transport's actual connecting,
rejected, timed-out and error state. The returned storage persists across calls.
This displays readiness; it adds no Ready button or guest network call.

The focused C query test passes for these status states, including closed
sessions and readiness changes. In-game title fit and refresh remain unverified
while the separate Vulkan launch failure prevents new visual validation.

## Custom settings layout repair

Later offscreen runtime captures `UC2-custom-LAN.png` and
`UC2-custom-mode-accepted.png` show two members and a working mode change,
but the original kind8 mode/map controls overlap at the first row. Their rich
list metadata remains gated by the removed online implementation; supplying
only text/index queries does not reconstruct that layout.

The experimental overlay now uses the existing kind6 ordinary choice widget
for commands57 and58, matching campaign difficulty, audio/language choices and
the adjacent multiplayer scalar settings in the same menu.bin. Exact 1.02
records: mode9df0 `(kind8,flags3)` and map9e60 `(kind8,flags1)` become
`(kind6,flags0)`. Their localized label pointers/IDs, native command IDs, column0
and width400 are unchanged. Four bytes change in total; strict descriptor
validation occurs before mutation. Guard rejection and compatibility with the
Ready overlay patch pass. This repair awaits its own new rendered capture;
the earlier screenshots are evidence for the defect, not the fix.

The subsequent parent runtime capture confirms that kind6 renders separate
mode/map rows and allows selection. The parent handles the duplicate left-hand
label by returning the field name in the text query and the selected value in
the item-text query.

Ready was visible in column1 but Up/Down stayed within column0. Rizin confirms
the vertical navigation loop at5a1cc1 reads each control descriptor+58 and
compares it with the current column at5a1cdf..5a1ce1. The layout grouping at
5a297d reads the same field. Both Ready descriptors now move from column1 to
column0 while retaining width320, callback identity and command48. Existing
flags are preserved. This exact column change is guarded before mutation;
invalid-column rejection and compatibility with the setting conversion pass.
Evidence: `work/rizin-mp/ui-ready-navigation-1.02.asm`. Ready focus/click after
this move awaits the next parent runtime validation.

## Native UDP v2 preparation diagnostic

The current transport uses **protocol v2**. Version 1 datagrams are rejected;
rebuild both game instances and `uc2mp-lan-peer` together. The binary packet
layout is bounded serialization, not a C structure or guest-memory image.
The shared bearer token is still sent in clear and is intended for trusted LAN.

`Uc2mpLobby.preparation` holds the phase, increasing attempt ID, fixed local
monotonic deadline and two roster-index ACK masks. The transitions are:

1. Host begins `PREPARING` only with a known nonzero mode/map from the native
   catalog and every participant ready. Configuration, names, new membership
   and readiness changes are frozen. A departure cancels the attempt.
2. Every participant, including the host, explicitly acknowledges that attempt's
   `PREPARING` phase. Only the host may authorize `LOADING`, after all ACKs and
   fresh valid transport observations of every remote participant. Missing
   participants never force a late start.
3. Each participant explicitly acknowledges receipt of `LOADING`. This is an
   acknowledgment of permission, **not evidence that a map or avatar loaded**.
   The host or any participating client's failure report can cancel, returning
   to `LOBBY` with all ready and ACK bits cleared.

The preparation deadline is fixed at begin and limited to 120,000 ms. ACKs,
heartbeats and the transition to `LOADING` never renew it. Clients derive a
local deadline from the host's remaining budget and may shorten it, never
extend it for the same attempt. While the attempt is active, this bounded grace
replaces the ordinary peer timeout, so a temporary game-thread loading stall
does not immediately disconnect the group at 5 s. At deadline, the host cancels;
a client whose attempt expires closes its connection. Neither path starts a
match. A new host epoch/client nonce establishes a fresh attempt-ID namespace;
delayed old-epoch traffic and canceled-attempt ACKs cannot re-arm it.

`uc2mp_lan_prepare_can_block()` reports all loading-receipt ACKs on the host,
or the local receipt ACK confirmed in a host snapshot on a client. It is **not
a perfect distributed barrier**: the last UDP confirmation can be lost. A
future engine adapter must continue pumping during preparation, account for
the deadline and cancellation, and must not interpret this predicate as proof
that other engines are running. No engine call, Start button hookup, map load,
spawn or gameplay synchronization is implemented by these APIs;
`uc2mp_start()` remains `UC2MP_UNSUPPORTED`.

Build the existing `uc2mp-lan-peer` target, then run:

```text
python tools/test_uc2mp_lan_processes.py
```

The script retains the original two-process join/ready/leave smoke test and
adds an independent pair with `--prepare-test`. It generates a fresh token,
uses explicit loopback addresses, bounds subprocess waits and saves logs in
`work/local-lobby/lan-smoke/`. The diagnostic host selects catalog
`deathmatch` / `village-multi` with capacity 2; both peers automatically ready,
exchange both phase ACKs and verify loading permission. The client then
requests cancellation only after observing its confirmed loading ACK. Both
processes verify the same attempt returned to lobby with two members and zero
ready/ACK bits; the client leaves and the host verifies its removal before
exiting. The script checks ordered events, matching IDs and exit status from
both actual processes. It also verifies a missing peer fails within 3 s and
invalid preparation durations/duplicate flags are rejected.

Manual equivalent, in two terminals with the same newly generated HEX32 token:

```text
uc2mp-lan-peer --host 127.0.0.1 --port 3233 --token HEX32 --name Host --id 1 --seconds 30 --prepare-test
uc2mp-lan-peer --connect 127.0.0.1 --port 3233 --token HEX32 --name Peer --id 2 --seconds 30 --prepare-test
```

`--prepare-test` requires a duration from 3 to 120 seconds and is intended for
two diagnostic processes, not an invitation to start a match in the game.
Success exits 0 after cancellation; incomplete preparation exits 4. The ordinary
`--ready` mode remains available without this scenario. Tokens are omitted
from logs. `prepare-host.log` / `prepare-client.log` provide the successful
handshake evidence; `prepare-missing-peer.log` records the bounded failure.
Separate native model/transport tests cover dropped ACK retry, reordered or
malformed phases, wrong endpoint/epoch/attempt, deadline enforcement, simulated
blocking-load grace and reconnect after a host restart.
