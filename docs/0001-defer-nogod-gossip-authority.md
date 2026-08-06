# Decision: defer porting NoGod.lean's gossip/vector-clock zone authority

**Status:** decided (task #15's evaluation, not deferred silently)

## Context

`lean-rebac-core`'s `Rebac/core/NoGod.lean` (imported by `ReBAC.lean`,
ported in task #13) is a proven, coordinator-free gossip protocol for
zone-range consensus: vector clocks (`VClock`), Hilbert-range containment
(`ZoneRange`, `geometricAuthority`, `geometricInterest`), a hybrid logical
clock (`HLC`), and theorems that gossip-based range adoption preserves
`DisjointRanges` (no two zones claim overlapping authority) without a
central coordinator.

## Decision: not ported now

`zone-server-h2o` today hardcodes `z_id = 0` in
`src/transport/webtransport_server.c` -- it binds one UDP port and
services exactly one zone. There is no second zone to route between, no
gossip peer to exchange `ZoneRange` claims with, and no vector clock
needed to order events that, by construction, all originate from the one
process that has them. Porting the full `VClock`/gossip/`HLC` system now
would be building structure for a need that does not exist yet -- the
same mistake this plan already corrected once (see the plan file's
recollection of Kent Beck's YAGNI argument: the cost is not the typing
effort, it's committing to a guessed structure and pulling the cost
forward before the need, and the value, actually arrives).

## What ports cleanly when multi-zone routing is real work

When a second zone exists, the reusable piece is narrow: `ZoneRange`
(a `{zoneId, lo, hi}` Hilbert-code interval) and `ZoneRange.contains` /
`geometricAuthority` (find the zone whose range contains a given Hilbert
code) -- about ten lines of pure C, directly portable the same way
`rebac.c` was. The gossip protocol (`NodeView`, `GossipMsg`,
`VClock.merge`, `HLC.advance`/`HLC.merge`) is what makes authority
assignment coordinator-free *across multiple zone-server processes* --
worth porting once there are multiple processes to coordinate, not
before.

## Revisit when

Task #8 or #9 introduces a second concurrently-running zone (not just a
second `z_id` value handled by the same process/transaction context).
At that point, port `ZoneRange`/`geometricAuthority` first (needed
immediately), and the gossip/`VClock`/`HLC` layer only once there is a
real second process to gossip with.
