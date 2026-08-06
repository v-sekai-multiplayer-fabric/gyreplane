# zone-server-h2o

Native `libh2o` + FoundationDB (FDB) zone server for the V-Sekai multiplayer
fabric, replacing the Godot `FabricZone`/`FabricZoneJournal`/`FabricMMOGZone`
engine (`V-Sekai-fire/multiplayer-fabric-build`, `godot/modules/multiplayer_fabric/`)
on the **server** side. The client stays Godot engine, unchanged; only the
authoritative zone server moves.

## Status

QUIC transport and H3/WebTransport session negotiation are both wired
(`src/transport/webtransport_server.c`, `src/transport/wt_session.c`),
driving a real FDB-backed `ZoneTick` (`src/zf_zonetick.c`) across a small
fabric of zones (`WT_SERVER_ZONE_FABRIC_SIZE`, currently 4) — not a
single hardcoded zone. Not yet done: TLS cert/key (still `NULL`/`NULL`,
so unauthenticated), physics/IK, and cutover from the Godot deployment.
The event-loop, worker-pool, and FDB scaffold were seeded from
[`weftspun/h2o-bench-tpcc`](https://github.com/weftspun/h2o-bench-tpcc)
(that repo's TPC-C benchmark work is unrelated and stays there; only the
reusable infrastructure and the unimplemented `zonefabric` scenario design
carried forward here).

### Concurrency and scaling

Each zone in the fabric gets its own independent FDB transaction
(`zf_zonetick_run`, one call per zone, no cross-zone lock or shared
state) — RFD 0002's own core-scaling argument ("no cross-zone
conflicts... near-linear core scaling, unlike TPC-C") depends on exactly
that independence, and `test/unit/test_zf_kv_multi_zone.c` proves the
FDB keyspace isolation it relies on (6 test zones, no entity key from one
zone ever falls inside another's range). **Not yet measured**: actual
linear scaling of concurrent zone ticks needs a running FDB cluster and
a load generator (RFD 0013's `wrk` harness) — this repo's test suite
cannot exercise that without live infrastructure, so it's a real,
open verification gap, not a claim made and left unchecked.

"Fabric of zones" here means *one process handling several zones* — it
does not yet mean multiple zone-server processes coordinating with each
other. That's a distinct, larger question (`docs/0001-defer-nogod-gossip-authority.md`),
deliberately deferred until there's a second process to coordinate with.

## Design provenance

- Event loop, worker pool, FDB async plumbing, SPSC ring: ported as-is from
  `h2o-bench-tpcc`'s `src/` (RFD 0005 actor-lite architecture, RFD 0011 async
  FDB callback chain).
- **Transport is `picoquic` + `picotls`, not `h2o`'s own QUIC stack.**
  `h2o` (pinned by `h2o-bench-tpcc`) has no WebTransport/datagram support at
  all -- confirmed by searching its source tree. The Godot client's
  `WebTransportPeer` (`V-Sekai-fire/multiplayer-fabric-build`,
  `godot/modules/http3/`) is built on vendored `picoquic` + `picotls`, which
  *does* have working WebTransport, datagrams, and MASQUE support
  (`picohttp/webtransport.c`, `picoquictest/datagram_tests.c`,
  `picohttp/picomask.c`). `thirdparty/picoquic` and `thirdparty/picotls` here
  are git submodules pinned to the exact commits that fork vendors
  (`790e973b`, `3b4d709f`), so client and server share one proven QUIC
  stack. See `cmake/picoquic.cmake` (mirrors that module's `SCsub`) and
  `src/transport/webtransport_server.c` (not yet implemented -- the actual
  wiring point for task #11).
- Entity/migration/ghost/journal shape: modeled on the real (if
  never-yet-invoked) `FabricZone` C++ engine in
  `V-Sekai-fire/multiplayer-fabric-build`, not built from a blank slate.
- Physics/IK: ports [`sinew-mocap/solve`](https://github.com/sinew-mocap/solve)
  (FK + LBS skinning), constrained by the `lean-humanoid-rom` and
  `swing-twist-kusudama` proofs.
- Entity/ReBAC types: generated from `lean-entity-packet` and
  `lean-rebac-core` rather than hand-duplicated per language.
  `src/gen/xr_grid_entity_packet.{c,h}` is a direct C transcription of
  `lean-entity-packet`'s `EntityPacket/Codec.lean` (100-byte packet,
  little-endian, no floats on the wire), differentially tested against
  that repo's 64 Plausible-verified golden vectors
  (`test/unit/test_xr_grid_entity_packet.c` -- byte-identical round-trip,
  not just field equality) and now the real storage type in `zf_kv.h`'s
  `zf_entity_val_t` (the float-double placeholder is gone). `zf_zonetick.c`
  ticks in fixed-tick integer micrometers, matching the wire's actual
  semantics (velocity is a per-tick displacement, not a continuous rate).
  `src/gen/rebac.{c,h}` ports `lean-rebac-core`'s `Rebac/core/ReBAC.lean`
  `rebacCheck` -- a pure predicate over `Relation`/`Action` ranks, tested
  against the Lean source's own proved theorems (`rebac_empty_denied`,
  `rebac_public_observe`, the owner-only `.modify` boundary). Not yet
  wired into the transport layer -- the geometric-authority routing
  ("which zone evaluates the check") it depends on is task #8/#9's
  zone-authority work. `Rebac/core/NoGod.lean`, which `ReBAC.lean`
  imports, turned out to be a much bigger find than task #13 alone: a
  gossip-based, coordinator-free vector-clock consensus system for zone
  authority/interest -- directly relevant to task #8's zone-authority and
  entity-migration work, not yet acted on.
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
