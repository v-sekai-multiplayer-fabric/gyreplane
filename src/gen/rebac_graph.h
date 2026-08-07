#ifndef REBAC_GRAPH_H_
#define REBAC_GRAPH_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "rebac.h"

/*
 * Unified ReBAC: the existing rank check (rebac.c, ported from
 * v-sekai-multiplayer-fabric/lean-rebac-core's Rebac/core/ReBAC.lean)
 * plus a graph-based check, ported from taskweft/taskweft's own
 * lean/Planner/ReBACCorrectness.lean and ReBACGoal.lean (proved
 * there, exposed to C++ via taskweft/nif's standalone/tw_rebac.hpp,
 * TwReBAC::check_expr/check_base). Confirmed by direct source read:
 * these are two independent Lean formalizations, not one lineage
 * split across two repos. lean-rebac-core's ReBAC.lean has zero
 * references to graphs, RelationExpr, union/intersection/difference,
 * or check_expr; it is a fixed five-rank total order (public=0 ..
 * owner=4) under No-God zone-authority theory. taskweft's
 * ReBACCorrectness.lean formalizes a Zanzibar-style relation graph
 * with union/intersection/difference/tuple_to_userset over arbitrary
 * named relations, no total order assumed.
 *
 * The graph model is strictly more powerful: a fixed rank check is
 * one graph with one edge per relation the claim holds, evaluated as
 * a union of "does subject hold relation R to object" base checks
 * for every R at or above the action's minimum rank. rebac_check()
 * (rebac.c) is kept as the fast, common-case path for a flat claim
 * against one already-known object; rebac_check_graph() is the
 * general path for delegation and transitive group membership,
 * neither of which a flat rank list can express.
 * rebac_check_graph_matches_flat() below is the equivalence proof,
 * run as a real test, not just asserted in a comment.
 */

/* Edge relation kinds. The five ranked relations from rebac.h double
 * as direct-hold relation kinds here (REBAC_GRAPH_REL_PUBLIC ..
 * REBAC_GRAPH_REL_OWNER, same rank order). Two structural kinds are
 * added on top, direct ports of taskweft's IS_MEMBER_OF and CONTROLS
 * handling in TwReBAC::check_base:
 *
 *   MEMBER_OF:    subject -> group. Transitive: if subject is a
 *                 member of group, and group holds the needed
 *                 relation to object, subject holds it too.
 *   DELEGATED_TO: object -> subject (edge direction inverted, per
 *                 taskweft's own "Delegation inversion for CONTROLS"
 *                 comment). Grants subject an owner-equivalent hold
 *                 on object without a direct owner edge -- one
 *                 player deputizing another to manage their own
 *                 entity, the one case a flat per-claim rank list
 *                 cannot express at all.
 */
typedef enum {
    REBAC_GRAPH_REL_PUBLIC = REBAC_RELATION_PUBLIC,
    REBAC_GRAPH_REL_INSTANCE_MEMBER = REBAC_RELATION_INSTANCE_MEMBER,
    REBAC_GRAPH_REL_FRIEND = REBAC_RELATION_FRIEND,
    REBAC_GRAPH_REL_GUILD_MEMBER = REBAC_RELATION_GUILD_MEMBER,
    REBAC_GRAPH_REL_OWNER = REBAC_RELATION_OWNER,
    REBAC_GRAPH_REL_MEMBER_OF,
    REBAC_GRAPH_REL_DELEGATED_TO,
} rebac_graph_rel_t;

typedef struct {
    uint64_t subject;
    uint64_t object;
    rebac_graph_rel_t relation;
} rebac_edge_t;

/* taskweft/nif's own default (lib/taskweft.ex's `fuel \\ 8`, TwState's
 * own `rebac_fuel = 8`); reused here rather than invent a different
 * default for the same kind of bounded-recursion parameter. */
#define REBAC_GRAPH_DEFAULT_FUEL 8

/* Direct port of TwReBAC::check_base, specialized to this project's
 * fixed relation set (no RelationExpr tree needed yet -- nothing in
 * this project's own action model uses union/intersection/difference
 * today; the graph engine's real value here is MEMBER_OF/DELEGATED_TO
 * traversal, not boolean expression combinators). fuel bounds
 * transitive MEMBER_OF recursion, exactly as taskweft's own p_fuel
 * parameter does, so a cyclic membership graph cannot loop forever. */
bool rebac_check_graph(const rebac_edge_t *edges, size_t count,
                        uint64_t subject, uint64_t object,
                        rebac_action_t action, int fuel);

/* Equivalence proof: build a one-object graph from a flat relations
 * list (one edge per relation, same subject/object), and confirm
 * rebac_check_graph() agrees with rebac_check() for every action.
 * Exercised as a real test in test/unit/test_rebac_graph.c, not only
 * documented here. */
bool rebac_check_graph_matches_flat(const rebac_relation_t *relations, size_t count,
                                     rebac_action_t action);

#endif
