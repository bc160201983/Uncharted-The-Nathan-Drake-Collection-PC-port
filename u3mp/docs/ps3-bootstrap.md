# U3 PS3 bootstrap comparison

Date: 2026-10-25. Static analysis; no game was launched or binary modified.

## New findings

The PS3 online bootstrap explicitly loads the `net` data package before it reads multiplayer configuration. This provides a concrete model for repairing the missing PS4 registry initialization instead of bypassing null checks.

`NetInit` was located at PS3 virtual address `0x002901e0` by a PowerPC TOC reference to the literal `NetInit`. The independently recovered TOC is `0x011c2070`. Its sequence includes:

1. Resolve `*skel-retarget-tables*` (`0xfe014fd2`) and reset entries.
2. Push system-module configuration `multi-player` through `0x007132bc`.
3. Release three prior resource references through `0x009af740`.
4. Call `0x009afb74(0x4240ef2e, &tag25)`; CRC `0x4240ef2e` identifies `net`.
5. Resolve `*world-levels*` (`0x1020810b`), then read `*net-quit-penalty-game*` (`0xd34ac18e`) and `*net-fight-damage*` (`0x6c912553`).
6. Continue into NetCtl/identity checks and local-player setup.

The inferred package-load identification is supported by callee structure, not just the name: `0x009afb74` calls resource enqueue `0x009ae314`, repeatedly pumps `0x009aeb08`, searches the resource map for the requested hash, and waits for state at entry +0x0c to become nonzero. `0x009ae314` recursively acquires dependency hashes and increments an existing resource's reference count. Companion `0x009af740` decrements references and recursively releases dependencies. No semantic meaning is asserted for tag value 25 beyond the observed argument.

The NetInit call to `0x0031c0a4(5)` is a contiguous memory reservation helper, not a networking API. It must not be used as evidence of live transport.

## Character resource format comparison

A PS3 resource was independently extracted from the supplied GOTY archive into the task's scratch directory:

`goty-patch.psarc::build/main/bin/dc1/net.bin`

The corresponding DCI calls this package `net-patch10` and explicitly exports `*custom-characters*`. This disc patch is separate from the supplied 1.19 executable and the historical 1.19 runtime's downloaded net18 data.

| Property | Supplied PS3 GOTY patch | Native PS4 resource found by parallel analysis |
|---|---:|---:|
| Byte order | Big endian | Little endian |
| Symbol-record size | 12 bytes | 16 bytes |
| Character symbol-record offset | `0x1230` | `0x12a8` |
| Character array offset | `0x45a1c` | `0x315c0` |
| Array-header size | 8 bytes | 16 bytes |
| Character record size | `0x3c` | `0x70` |
| Character count | 76 | 57 |

Both symbol records use hash `0xbd3bb3b9` and type hash `0x4b7415bd`. First records agree on name/ID/category:

- hunter: ID `0xc4c464f0`, category 0
- pirate: ID `0xb02fb173`, category 1
- coop-cutter: ID `0xbc83cdc6`, category 2

The PS3 resource therefore corroborates the interpretation of PS4 character data while also demonstrating why PS3 binary data cannot be copied directly into the PS4 runtime.

A bounded scan found no direct big-endian `0xbd3bb3b9` value and no nearby PowerPC lis/ori or lis/addi construction in the PS3 flattened executable. This is not evidence that the feature is absent: its data package exports the symbol, and PS3 may derive it through other constants or runtime hashing.

## Evidence and limitations

- `ps3-resource-schema.json` includes the extracted PS3 resource's SHA-256 and all 76 decoded character names/IDs/categories.
- `ps3-netinit.c` is fresh Ghidra 12.2 decompilation from a copied saved project, processed with `-readOnly -noanalysis`.
- `ps3-net-resource-route.c` and `ps3-net-resource-enqueue.c` contain supporting loader/lookup decompilation.
- `ps3-bootstrap-xrefs.json` records the custom PowerPC TOC resolution.
- Required-symbol names were verified using the target's own MSB-first CRC32 lookup table, seed 0, and the native PS4 DCI exports; shared names need not imply identical layouts.
- The historical PS3 TTY log supports the order of online module switching, live net18 fetch, and later private-match startup. No remote URL was fetched during this investigation.
- No patch, runtime resource registration, restored menu, or working multiplayer session is claimed by this sub-investigation.
