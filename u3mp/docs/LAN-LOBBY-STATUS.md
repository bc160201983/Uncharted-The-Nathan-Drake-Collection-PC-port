# U3 LAN lobby restoration

Original working notes. All edits and new builds use this location; original game installations remain unchanged.

Manual controller build: root `Test-Uncharted3-LAN-Lobby.cmd`. Frozen image `builds/lan-lobby-v1/boot-linked.bin`, SHA256 f5c898ebfa403c620c8101823a4fcbf116ba3e6b63c3b223d156726dbc8b9ab3. Windowed, no replay/scripted input, no timeout. Profile store `user/lan-profiles.v1` retains the user's Hunter and Johnny profiles.

Native run lan-profiles-1792956112025939600 verified selected profile -> native Connecting page -> net.bin load and correctly scoped pool initialization -> lobby with six labels and the chosen local name. Duplicate text columns were corrected and captured in evidence/lan-lobby-columns-fixed.png. Manual launch baseline-1792956325850598700 exited normally after 183 seconds with two profiles preserved.

Remaining visible differences: desert background, no restored hangar/Drake scene, no icons/rank/stats. No hosting, joining, session backend or playable multiplayer has been demonstrated. Two instances cannot connect.

Navigation regression reported by user: lobby begins on player name and moving toward main menu backs out. Development tracing found operation11 row-selectability output was left zero; all main rows were disabled. The native click registry and Options routing are implemented behind the mutable development helper; further native test required before promotion. Existing frozen build is not changed by source edits.

Evidence: ps4-showpage-focus.c, ps4-list-control-input.c, ps4-datasource-method-range.c, lobby-datasource-adapter.md. Note PS3 enum b3 resolves to discovery-room rows at 0x342d84, not the remaster's main menu; enum values must not be assumed identical across builds.

Latest manual snapshot: builds/lan-lobby-v2, SHA256 94b3bd8968af3368fb5c15706741138cda85a4fd9b181952cdf9fe73ede200bd. Root launcher now points here. Row selectability change did not demonstrate navigation success in native test lan-profiles-1792957192026704000. Registry read showed 93 entries, original LAN callback present but added main callback absent: menu construction occurs before native registration completes. Next repair must register after table initialization, then verify focus and actual Options transition. No fix is claimed.
