# U3 missing character resource identified

Hash `0xbd3bb3b9` is `*custom-characters*`. This is verified using the game's actual CRC32 table at image offset `0x13a6ff0`: polynomial `0x04c11db7`, seed zero, no final XOR, case sensitive. As a control, `TaskManagerOnline` hashes to the independently known `0xb7b3c967`.

The native PS4 resource already exists at `u3data/build/main/dc1/ps4/net.bin`; its companion `net.dci` exports this exact name. A symbol definition at file offset `0x12a8` contains the name hash, type hash `0x4b7415bd`, and a value offset `0x315c0`. The value is a 57-entry character array whose records begin at `0x315d0` and have stride `0x70`. Each record has a name pointer at +0, identifier at +8, a second identifier at +0x10, and category at +0x14. The first entries are hunter/category 0, pirate/category 1, and coop-cutter/category 2.

The existing crash at guest offset `0x99610b` follows a null registry lookup for this symbol. The meaningful next fix is to restore the game's load/registration route for native PS4 `net.bin` and its imports before the multiplayer menu runs. This is evidence against treating the array itself as removed or fabricating a replacement. Static inspection does not establish that loading this resource alone resolves all initialization failures.

The resource header is `00CD`, version 1, with a 405-entry sorted symbol table at +0x18. Each 16-byte entry stores hash:u32, type hash:u32, and value offset:u64. Offset +8 points at post-data metadata at 0x4ec90. That metadata's encoding remains unresolved; pointers in file are offsets and require the normal loader's relocation.

`ss-multiplayer.dci` exports `ss-test-net-go`, `ss-attack-object-when-ignored`, and `ss-cowboy-test-coop`. It does not define the missing character array. Literal-only searches of four title-screen/world-multiplayer packages did not recover a load reference; package metadata may use hashes.

Full scan boundaries, occurrences, definitions, and all 57 decoded character records are in `hash-resource-findings.json`. No game was launched and no original files were modified.
