# zone-server-h2o

Native `libh2o` + FoundationDB (FDB) zone server for the V-Sekai multiplayer
fabric, replacing the Godot `FabricZone`/`FabricZoneJournal`/`FabricMMOGZone`
engine (`V-Sekai-fire/multiplayer-fabric-build`, `godot/modules/multiplayer_fabric/`)
on the **server** side. The client stays Godot engine, unchanged; only the
authoritative zone server moves.

## Status

Pre-transport spike. No WebTransport/QUIC listener yet. The event-loop,
worker-pool, and FDB scaffold below are seeded from
[`weftspun/h2o-bench-tpcc`](https://github.com/weftspun/h2o-bench-tpcc)
(that repo's TPC-C benchmark work is unrelated and stays there; only the
reusable infrastructure and the unimplemented `zonefabric` scenario design
carried forward here).

First working milestone (in progress): a WebTransport datagram round-trip
plus a bare `ZoneTick` (`position += velocity * dt`, no physics) — see
`src/main.c`'s `TODO(task #11)`.

## Design provenance

- Event loop, worker pool, FDB async plumbing, SPSC ring: ported as-is from
  `h2o-bench-tpcc`'s `src/` (RFD 0005 actor-lite architecture, RFD 0011 async
  FDB callback chain).
- Entity/migration/ghost/journal shape: modeled on the real (if
  never-yet-invoked) `FabricZone` C++ engine in
  `V-Sekai-fire/multiplayer-fabric-build`, not built from a blank slate.
- Physics/IK: ports [`sinew-mocap/solve`](https://github.com/sinew-mocap/solve)
  (FK + LBS skinning), constrained by the `lean-humanoid-rom` and
  `swing-twist-kusudama` proofs.
- Entity/ReBAC types: generated from `lean-entity-packet` and
  `lean-rebac-core` rather than hand-duplicated per language.
- Memory safety: built with [Fil-C](https://github.com/pizlonator/fil-c)
  once the toolchain is wired in (task #3) — this process parses untrusted
  WebTransport input directly from clients.

Design decisions land as dated entries in
[`multiplayer-fabric-manuals/decisions/`](https://github.com/v-sekai-multiplayer-fabric/multiplayer-fabric-manuals/tree/main/decisions),
not in a local `rfd/` folder — see that repo for the carried-over RFD content
(zonefabric scaling, actor-lite architecture, FDB keyspace design, PERT
critical path, etc.) once ported.

## Build

```sh
cmake -B build && cmake --build build
```

Requires `libh2o` (evloop build), OpenSSL, and the FoundationDB C client
(`libfdb_c`) on the include/library path. See `CMakeLists.txt`.

## Verification

- `test/cbmc/spsc_harness.c` — CBMC proof of the SPSC ring buffer, ported
  from `h2o-bench-tpcc` (RFD 0008).
- `test/verification/` — Lean 4 + Plausible specification harness
  (`ZoneVerification.Spsc`), same provenance. Zonefabric-specific
  invariants (entity migration, ghost consistency, journal replay) land
  here as those features are built.
