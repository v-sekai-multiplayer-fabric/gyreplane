# zone-server-h2o

This is a native `libh2o` + FoundationDB (FDB) zone server for the V-Sekai
multiplayer fabric. It replaces the Godot
`FabricZone`/`FabricZoneJournal`/`FabricMMOGZone` engine
(`V-Sekai-fire/multiplayer-fabric-build`, `godot/modules/multiplayer_fabric/`)
on the **server** side. The client stays Godot engine, unchanged. Only the
authoritative zone server moves.

## Status

This repo wires QUIC transport and H3/WebTransport session negotiation
(`src/transport/webtransport_server.c`, `src/transport/wt_session.c`). These
drive a real FDB-backed `ZoneTick` (`src/zf_zonetick.c`). Each process runs
exactly one zone. The zone ID comes from `main.c`'s required `-z<zone_id>`
flag, not a hardcoded value.

A zone fabric means multiple processes. Each process runs one zone (1
process : 1 zone). This matches `zone-server/AGENTS.md`'s deployment
shape: one UDP port per zone instance, up to 100 concurrent zones.
Avatar IK (`sinew-mocap/solve`'s `Align.lean`) is wired and tested.
Entity and prop physics used MuJoCo for a while; that dependency is
dropped now in favor of Godot's own Jolt physics (see
`docs/0002-defer-mink-port-keep-sinew-mocap-solve.md`'s Revision 3).
The vendored MuJoCo build and its `mj_physics.c` wiring moved to
`v-sekai-multiplayer-fabric/mujoco-riscv64`.

Three items are not done yet:
- The TLS cert and key are still `NULL`/`NULL`, so the server is
  unauthenticated.
- No real `cmake --build` against the actual linked libraries ran in this
  repo's own development yet. CI now does this build
  (`.github/workflows/real-build.yml`). Until this section says otherwise,
  treat it as freshly wired, not yet fully green.
- Cutover from the Godot deployment did not happen yet.

The event-loop, worker-pool, and FDB scaffold came from
[`weftspun/h2o-bench-tpcc`](https://github.com/weftspun/h2o-bench-tpcc). That
repo's TPC-C benchmark work is unrelated and stays there. Only the reusable
infrastructure, and the unimplemented `zonefabric` scenario design, carried
forward here.

### Concurrency and scaling

Each zone-server-h2o process runs its own FDB transaction per tick
(`zf_zonetick_run`, one call per process, one zone). RFD 0002's own
core-scaling argument depends on exactly this: "each core processes
independent zones... near-linear core scaling, unlike TPC-C where
district-level conflicts cause retries." `test/unit/test_zf_kv_multi_zone.c`
proves the FDB keyspace isolation many such processes rely on. The test uses
6 test zone IDs. No entity key from one zone ever falls inside another
zone's key range. So no process can ever read or write another zone's data
by accident.

**Not yet measured**: actual linear scaling across many concurrent
zone-server-h2o processes. This measurement needs a running FDB cluster,
several deployed processes, and a load generator (RFD 0013's `wrk`
harness). This repo's test suite cannot exercise that without live
infrastructure. This is a real, open verification gap, not a claim made and
left unchecked.

Coordinating many zone-server-h2o processes is a distinct, larger question.
Two examples of this: deciding which process owns which zone, and moving
an entity from one zone's process to another. This question is tracked in
`docs/0001-defer-nogod-gossip-authority.md`. The team deliberately deferred
this question. The team needs to revisit it now: the deployment model is
confirmed as multiple processes, not a single process looping over several
zones.

## Design provenance

- Event loop, worker pool, FDB async plumbing, SPSC ring: ported as-is from
  `h2o-bench-tpcc`'s `src/` (RFD 0005 actor-lite architecture, RFD 0011 async
  FDB callback chain).
- **Transport is `picoquic` + `picotls`, not `h2o`'s own QUIC stack.**
  `h2o` (pinned by `h2o-bench-tpcc`) has no WebTransport or datagram support
  at all. A search of its source tree confirms this. The Godot client's
  `WebTransportPeer` (`V-Sekai-fire/multiplayer-fabric-build`,
  `godot/modules/http3/`) is built on vendored `picoquic` + `picotls`, which
  *does* have working WebTransport, datagrams, and MASQUE support
  (`picohttp/webtransport.c`, `picoquictest/datagram_tests.c`,
  `picohttp/picomask.c`). `thirdparty/picoquic` and `thirdparty/picotls` here
  are git subtrees checked in at the exact commits that fork vendors
  (`790e973b`, `3b4d709f`). This way, client and server share one proven
  QUIC stack. See `cmake/picoquic.cmake` (mirrors that module's `SCsub`),
  `src/transport/webtransport_server.c` (the QUIC transport bridge into
  h2o's evloop), and `src/transport/wt_session.c` (H3/WebTransport session
  negotiation on top of it, task #12).
- Entity, migration, ghost, and journal shape: modeled on the real (if
  never-yet-invoked) `FabricZone` C++ engine in
  `V-Sekai-fire/multiplayer-fabric-build`, not built from a blank slate.
- Physics: used [MuJoCo](https://github.com/google-deepmind/mujoco) 3.11.0
  for entity and prop contact physics (collisions, joints, forces) for a
  while. Dropped in favor of Godot's own Jolt physics -- see
  `docs/0002-defer-mink-port-keep-sinew-mocap-solve.md`'s Revision 3.
  The vendored build moved to
  [`v-sekai-multiplayer-fabric/mujoco-riscv64`](https://github.com/v-sekai-multiplayer-fabric/mujoco-riscv64).
- Avatar IK and posing: [`sinew-mocap/solve`](https://github.com/sinew-mocap/solve)'s
  own `Align.lean` (Kabsch-style rotation fitting, in `src/gen/sinew_align.c`),
  not `kevinzakka/mink`'s QP-based approach. The team evaluated `mink` and
  deferred it (`docs/0002-defer-mink-port-keep-sinew-mocap-solve.md`). A test
  against `Align.lean`'s own `AlignTest.lean` known-rotation-recovery check
  verifies this port. The result matches the exact expected value in
  floating-point arithmetic, not just a value within tolerance.
- Entity and ReBAC types: generated from `lean-entity-packet` and
  `lean-rebac-core`, not hand-duplicated per language.
  `src/gen/xr_grid_entity_packet.{c,h}` is a direct C transcription of
  `lean-entity-packet`'s `EntityPacket/Codec.lean` (a 100-byte packet,
  little-endian, no floats on the wire). A differential test checks it
  against that repo's 64 Plausible-verified golden vectors
  (`test/unit/test_xr_grid_entity_packet.c`, a byte-identical round-trip
  check, not just a field-equality check). This codec is now the real
  storage type in `zf_kv.h`'s `zf_entity_val_t`. The float-double
  placeholder is gone.

  `zf_zonetick.c` ticks in fixed-tick integer micrometers, to match the
  wire's actual meaning: velocity is a per-tick displacement, not a
  continuous rate.

  `src/gen/rebac.{c,h}` ports `lean-rebac-core`'s `Rebac/core/ReBAC.lean`
  `rebacCheck`, a pure predicate over `Relation`/`Action` ranks. Tests
  check it against the Lean source's own proved theorems
  (`rebac_empty_denied`, `rebac_public_observe`, the owner-only `.modify`
  boundary). This predicate is not wired into the transport layer yet.
  The geometric-authority routing it depends on ("which zone evaluates
  the check") is task #8/#9's zone-authority work.

  `Rebac/core/NoGod.lean`, which `ReBAC.lean` imports, turned out to be a
  much bigger find than task #13 alone. It is a gossip-based,
  coordinator-free vector-clock consensus system for zone authority and
  interest. This system is directly relevant to the multiple-processes
  coordination question above. The team has not acted on it yet.
- Memory safety: built with [Fil-C](https://github.com/pizlonator/fil-c) in
  CI (`.github/workflows/build-filc.yml`, task #3). This process parses
  untrusted WebTransport input directly from clients. The FFI boundary
  against `h2o`/`libfdb_c` (both still stock-compiled) is not resolved yet.

Design decisions land as dated entries in
[`multiplayer-fabric-manuals/decisions/`](https://github.com/v-sekai-multiplayer-fabric/multiplayer-fabric-manuals/tree/main/decisions),
not in a local `rfd/` folder. See that repo for the carried-over RFD content
(zonefabric scaling, actor-lite architecture, FDB keyspace design, PERT
critical path, and so on) once ported.

## Build

```sh
cmake -B build && cmake --build build
```

This build requires `libh2o` (evloop build) on the include/library path.
`libh2o`'s own build needs `libbrotli-dev` present at its build time. See
`h2o`'s `CMakeLists.txt`: it gates `libh2o-evloop`'s install rule on
finding Brotli.

This build also requires OpenSSL and the FoundationDB C client
(`libfdb_c`), both on the include/library path (see `CMakeLists.txt`).
It requires `mbedtls` too, built from source, not the system package.
Apt's `libmbedtls-dev` does not include `mbedtls_config.h`.

It also requires the vendored `thirdparty/` git subtrees (`picoquic`,
`picotls`, `QCBOR`), checked in directly (no separate init/fetch step),
built via `cmake/picoquic.cmake` / `cmake/qcbor.cmake`.
`.github/workflows/real-build.yml` runs this full build in CI. Check that
workflow's latest run for current status before assuming this build is
green.

## Verification

- `test/cbmc/spsc_harness.c`: a CBMC proof of the SPSC ring buffer, ported
  from `h2o-bench-tpcc` (RFD 0008).
- `test/verification/`: a Lean 4 + Plausible specification harness
  (`ZoneVerification.Spsc`), from the same source. Zonefabric-specific
  invariants (entity migration, ghost consistency, journal replay) will land
  here as those features are built.
