# Recovered pool initialization blueprint

The PS3 1.19 executable explicitly initializes the NetGameManager NPC tracker and pickup tables inside function 0x00305d2c, called by NetInit after entering allocation context/tag 27. This step precedes the online state machine.

| Table | PS3 manager offset | PS3 node stride | Capacity | Native PS4 manager offset | Native PS4 node stride |
|---|---:|---:|---:|---:|---:|
| NPC trackers | 0x58d4 | 0xb4 | 16 | 0x6520 | 0xc8 |
| Pickups | 0x5900 | 0x28 | 256 | 0x6590 | 0x40 |

Fresh PS3 functions 0x002fc270 and 0x002fc108 recover the initialization algorithm:

1. Round capacity up to a multiple of 16 for the hash-bucket count.
2. Allocate one 16-byte-aligned block: `(bucket_count + 1) * pointer_size + (capacity + 1) * node_stride`.
3. Put the bucket array first, followed immediately by the node pool.
4. Reserve the extra final node as the end sentinel; link its next/previous pointers to itself.
5. Store the sentinel pointer in the extra final bucket slot.
6. Link the real nodes into an empty/free doubly linked list; the first previous and last next pointers are null.
7. Clear the real buckets and mark the container initialized.

The corresponding native PS4 allocation sizes would be 3544 bytes for NPC trackers and 18512 bytes for pickups, using eight-byte pointers and the native node strides. Fresh PS4 initializer evidence at 0x1003850 and 0x125dc70 confirms that the bucket prefix itself is rounded up to 16 bytes, aligning the node pool as well. See POOL-BOOTSTRAP.md for the completed reconstruction and runtime result.

These sizes are derived from the observed PS3 allocation algorithm plus independently observed PS4 node strides. A native PS4 initializer using the same container family is being located to corroborate the remaining header fields and ownership/cleanup contracts before injection.

The available PS4 constructor at 0x71fe20 only zeros the table storage fields. The retained reset at 0x72ad80 already assumes storage exists. Neither is an allocator.

Evidence: `evidence/ps3-net-manager-allocation.c`, `evidence/ps3-net-manager-pool-helpers.c`, `evidence/ps4-next-bootstrap-fault.md`, `evidence/ps4-fixed-map-insert.c`.
