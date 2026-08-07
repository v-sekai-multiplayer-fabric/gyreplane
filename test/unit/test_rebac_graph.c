/*
 * Tests for the unified ReBAC graph check (src/gen/rebac_graph.c).
 * Two parts: equivalence against the existing flat rank check
 * (rebac.c, lean-rebac-core), and the two graph-only capabilities a
 * flat claim list cannot express -- transitive MEMBER_OF and
 * DELEGATED_TO -- ported from taskweft/taskweft's own
 * lean/Planner/ReBACCorrectness.lean via taskweft/nif's
 * TwReBAC::check_base. See src/gen/rebac_graph.h for provenance.
 */

#include "rebac_graph.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* Equivalence proof, run for real: every case test_rebac.c already
     * covers, re-checked through the graph engine. */
    {
        rebac_relation_t none[] = { 0 };
        assert(rebac_check_graph_matches_flat(none, 0, REBAC_ACTION_OBSERVE));
        assert(rebac_check_graph_matches_flat(none, 0, REBAC_ACTION_INTERACT));
        assert(rebac_check_graph_matches_flat(none, 0, REBAC_ACTION_MODIFY));

        rebac_relation_t pub[] = { REBAC_RELATION_PUBLIC };
        assert(rebac_check_graph_matches_flat(pub, 1, REBAC_ACTION_OBSERVE));
        assert(rebac_check_graph_matches_flat(pub, 1, REBAC_ACTION_INTERACT));
        assert(rebac_check_graph_matches_flat(pub, 1, REBAC_ACTION_MODIFY));

        rebac_relation_t inst[] = { REBAC_RELATION_INSTANCE_MEMBER };
        assert(rebac_check_graph_matches_flat(inst, 1, REBAC_ACTION_OBSERVE));
        assert(rebac_check_graph_matches_flat(inst, 1, REBAC_ACTION_INTERACT));
        assert(rebac_check_graph_matches_flat(inst, 1, REBAC_ACTION_MODIFY));

        rebac_relation_t owner[] = { REBAC_RELATION_OWNER };
        assert(rebac_check_graph_matches_flat(owner, 1, REBAC_ACTION_OBSERVE));
        assert(rebac_check_graph_matches_flat(owner, 1, REBAC_ACTION_INTERACT));
        assert(rebac_check_graph_matches_flat(owner, 1, REBAC_ACTION_MODIFY));

        rebac_relation_t mid[] = { REBAC_RELATION_PUBLIC, REBAC_RELATION_FRIEND,
                                    REBAC_RELATION_INSTANCE_MEMBER };
        assert(rebac_check_graph_matches_flat(mid, 3, REBAC_ACTION_OBSERVE));
        assert(rebac_check_graph_matches_flat(mid, 3, REBAC_ACTION_INTERACT));
        assert(rebac_check_graph_matches_flat(mid, 3, REBAC_ACTION_MODIFY));
    }

    /* Direct check still works when called through the graph engine
     * on a real multi-edge graph, not just the one-object helper
     * above. */
    {
        rebac_edge_t edges[] = {
            { .subject = 100, .object = 42, .relation = REBAC_GRAPH_REL_OWNER },
        };
        assert(rebac_check_graph(edges, 1, 100, 42, REBAC_ACTION_MODIFY, REBAC_GRAPH_DEFAULT_FUEL));
        assert(!rebac_check_graph(edges, 1, 999, 42, REBAC_ACTION_OBSERVE, REBAC_GRAPH_DEFAULT_FUEL));
    }

    /* Transitive MEMBER_OF: player 7 is a member of guild 200, guild
     * 200 holds INSTANCE_MEMBER on entity 42 -- player 7 inherits
     * enough rank for .interact, not .modify, with no direct edge of
     * its own to entity 42 at all. */
    {
        rebac_edge_t edges[] = {
            { .subject = 7, .object = 200, .relation = REBAC_GRAPH_REL_MEMBER_OF },
            { .subject = 200, .object = 42, .relation = REBAC_GRAPH_REL_INSTANCE_MEMBER },
        };
        assert(rebac_check_graph(edges, 2, 7, 42, REBAC_ACTION_OBSERVE, REBAC_GRAPH_DEFAULT_FUEL));
        assert(rebac_check_graph(edges, 2, 7, 42, REBAC_ACTION_INTERACT, REBAC_GRAPH_DEFAULT_FUEL));
        assert(!rebac_check_graph(edges, 2, 7, 42, REBAC_ACTION_MODIFY, REBAC_GRAPH_DEFAULT_FUEL));
    }

    /* Fuel bounds the MEMBER_OF chain: a long enough chain fails once
     * fuel runs out, exactly matching taskweft's own p_fuel <= 0
     * short-circuit in TwReBAC::check_base. */
    {
        rebac_edge_t chain[] = {
            { .subject = 1, .object = 2, .relation = REBAC_GRAPH_REL_MEMBER_OF },
            { .subject = 2, .object = 3, .relation = REBAC_GRAPH_REL_MEMBER_OF },
            { .subject = 3, .object = 4, .relation = REBAC_GRAPH_REL_MEMBER_OF },
            { .subject = 4, .object = 99, .relation = REBAC_GRAPH_REL_OWNER },
        };
        assert(rebac_check_graph(chain, 4, 1, 99, REBAC_ACTION_MODIFY, 8));
        assert(!rebac_check_graph(chain, 4, 1, 99, REBAC_ACTION_MODIFY, 2));
    }

    /* DELEGATED_TO: entity 42's real owner (player 5) delegates to
     * player 6, who holds no direct relation to entity 42 at all --
     * the one case a flat per-claim rank list cannot express, since
     * there is no single claim object holding both players' state. */
    {
        rebac_edge_t edges[] = {
            { .subject = 5, .object = 42, .relation = REBAC_GRAPH_REL_OWNER },
            { .subject = 42, .object = 6, .relation = REBAC_GRAPH_REL_DELEGATED_TO },
        };
        assert(rebac_check_graph(edges, 2, 6, 42, REBAC_ACTION_MODIFY, REBAC_GRAPH_DEFAULT_FUEL));
        /* Delegation only reaches .modify (owner-equivalent standing),
         * per this project's own "modify: owner only" boundary -- it
         * is not a general rank grant for lesser actions too. */
        rebac_edge_t bare_delegate[] = {
            { .subject = 42, .object = 6, .relation = REBAC_GRAPH_REL_DELEGATED_TO },
        };
        assert(!rebac_check_graph(bare_delegate, 1, 6, 42, REBAC_ACTION_OBSERVE, REBAC_GRAPH_DEFAULT_FUEL));
    }

    /* No edges, no membership, no delegation: denied, matching
     * rebac_empty_denied for the graph path too. */
    {
        assert(!rebac_check_graph(NULL, 0, 1, 2, REBAC_ACTION_OBSERVE, REBAC_GRAPH_DEFAULT_FUEL));
    }

    printf("rebac_graph: all checks passed (flat-rank equivalence, direct edges,\n"
           "             transitive MEMBER_OF, fuel bound, DELEGATED_TO inversion)\n");
    return 0;
}
