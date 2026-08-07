# Small MUD-mechanic domains, composable per taskweft's own file shape

Real, small, verified taskweft domains for the Middleham MUD
(`mud/guest/`, `mud/orchestrator/`, `mud/web/` alongside this
directory), one per MUD-relevant mechanic surveyed against
`fire/minizinc-2025-problems`, kept small on purpose and written so
they compose. These are content for this MUD specifically, not
general-purpose planner-library fixtures, so they live here rather
than in the vendored `thirdparty/taskweft-nif/`'s own `bench/`
directory.

- `mud_riddle_puzzle.jsonld`: a checkable puzzle-item mechanic (the
  `hitori`/`mondoku` logic-puzzle spirit). Grants NPC trust on a correct
  submitted answer, via a `"check"`-gated method alternative -- no new
  mechanism, the same pattern `thirdparty/taskweft-nif/bench/fixtures/
  domains/warehouse_domain_2robots_dock_preferred.jsonld`
  (`taskweft/nif#47`) already uses.
- `mud_room_visit_route.jsonld`: a multi-target visiting-order mechanic
  (the `tsptw`/`atsp` routing spirit). Method decomposition picks an
  efficient route across a small room graph (`gate`/`docks`/`market`),
  the same location-gated method pattern the vendored warehouse
  domain's `go_storage`/`go_shipping`/`go_dock` already use.
- `mud_npc_request_queue.jsonld`: a one-request-at-a-time capacity
  mechanic (the `work-task-variation` worker-capacity spirit). An
  `"eval"` precondition step in the action body (the same mechanism
  `blocks_world`'s own `a_pickup`/`a_stack` actions use) gates accepting
  a new request on the player not already carrying one.

## Composing them

A taskweft domain is a flat JSON document: `variables`, `actions`,
`methods` are each just a dict keyed by name. Since these three domains
use non-colliding names, and two (`mud_riddle_puzzle`'s `trust`,
`mud_room_visit_route`'s `at`) already reuse the same variable names
`warehouse_domain_2robots.jsonld` established, merging all three into
one combined MUD domain is a plain dict union across `variables`/
`actions`/`methods`, plus a `todo_list` that concatenates whichever of
the three mechanics a given session actually needs -- no new
composition mechanism needed, and nothing here is Godot- or
MUD-specific in the loader itself.

Each domain plans successfully on its own (verified with
`Taskweft.plan/1` directly against the vendored `thirdparty/
taskweft-nif/`, real plans produced for both the success and failure
paths where relevant). None of the three is wired into the MUD guest
(`mud/guest/mud_guest.cpp`) yet -- that is the approved NPC-planner
plan's own later stage, once `standalone/tw_planner.hpp` is embedded
into the guest ELF build.
