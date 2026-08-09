# zone-server-h2o examples

Three worked examples of the fabric's server-side roles, each an **h2o event
loop** driving **native cores**. No Godot runtime on the hot path: the interest
core is the codegen'd `predictive_bvh.h`, the packet is the codegen'd
`XRGridEntityPacket`, authority is the FDB-backed `zf_zonetick`. A Godot runtime
is only worth embedding when you actually need engine features like glTF/USD
I/O, which none of these do.

| Example | Role | Reuses |
|---|---|---|
| `authority_tick.c` | single-writer truth | `gen/rebac.h`, the Rivet API (Actor Runtime Socket) |
| `fanout.cpp` | interest-filtered fan-out (relay-tree leaf) | `predictive_bvh.h`, `gen/xr_grid_entity_packet.h` |
| `asset_distribution.c` | content-addressed asset CDN | casync (`idtxcli`), `gen/rebac.h` |

## Design notes (why they look the way they do)

- **Cheap or nasty** ([zguide ch.7](https://zguide.zeromq.org/docs/chapter7/)).
  The nasty path (millions/s entity packets) is the fixed 100-byte
  `XRGridEntityPacket` struct: `memcpy` + offset reads, no framing, no
  self-description, version pinned per connection. The cheap path (join, auth,
  interest changes) can afford CBOR/validation. Never CBOR the entity packet.
- **Batch by division, not delimiters.** A fan-out slice is back-to-back
  100-byte records; the receiver recovers count as `len / 100`. One write per
  subscriber per tick.
- **The actor has no database.** A Rivet actor never opens FDB, postgres, or a
  file. `authority_tick.c` reaches its state back through the **Rivet API** --
  the Actor Runtime Socket (SQLite over a Unix socket at
  `$ACTOR_RUNTIME_SOCKET_PATH`, length-prefixed vbare frames, `leaseKey`
  transactions). The engine behind that socket is what abstracts
  filesystem/postgres/fdb (Rivet's UniversalDB `driver/{postgres,rocksdb,fdb}`);
  the actor only ever speaks the API. Encode frames with the codec generated
  from `actor-runtime-socket-protocol/schemas/v1.bare`, never by hand.

## Seams (deliberately not wired here)

- **Fan-out transport.** WebTransport is not used in the container yet, so
  `fanout.cpp` delivers through a `fanout_sink_t` function pointer. The default
  sink is an ordinary h2o send; swap it for a WebTransport datagram sink once
  that path is in the container, with no change to the interest logic above it.
- **casync backend.** `asset_distribution.c` calls `casync_fetch_chunk`, the one
  seam onto `idtxcli` (`fabric-flow-adapters`) `fetch`/`verify`. Link its
  library or shell out; the handler, addressing, and authorization do not care
  which.

## Building

These are meant to be added as CMake targets beside `src/`, sharing its
event-loop, worker-pool, and FDB bootstrap (`src/main.c`, `src/event_loop.c`).
`fanout.cpp` is C++ because `predictive_bvh.h` is a C++ header (templated
`AabbT<int64_t>`); the repo already compiles C++ (`src/sandbox/*.cpp`).
