/*
 * Unified ReBAC graph check. See rebac_graph.h for provenance and scope.
 */

#include "rebac_graph.h"

static bool rank_holds(rebac_graph_rel_t relation, rebac_relation_t min_relation)
{
    switch (relation) {
    case REBAC_GRAPH_REL_PUBLIC:
    case REBAC_GRAPH_REL_INSTANCE_MEMBER:
    case REBAC_GRAPH_REL_FRIEND:
    case REBAC_GRAPH_REL_GUILD_MEMBER:
    case REBAC_GRAPH_REL_OWNER:
        return (rebac_relation_t)relation >= min_relation;
    case REBAC_GRAPH_REL_MEMBER_OF:
    case REBAC_GRAPH_REL_DELEGATED_TO:
        return false; /* structural edges, not a rank hold themselves */
    }
    return false;
}

static bool check_direct_and_member_of(const rebac_edge_t *edges, size_t count,
                                        uint64_t subject, uint64_t object,
                                        rebac_relation_t min_relation, int fuel)
{
    if (fuel <= 0) {
        return false;
    }

    /* Direct edge: subject holds a ranked relation to object already
     * at or above the action's minimum rank. */
    for (size_t i = 0; i < count; i++) {
        if (edges[i].subject == subject && edges[i].object == object &&
            rank_holds(edges[i].relation, min_relation)) {
            return true;
        }
    }

    /* Transitive MEMBER_OF: direct port of TwReBAC::check_base's own
     * IS_MEMBER_OF handling -- if subject is a member of some group,
     * and that group itself holds the relation to object, subject
     * inherits it. */
    for (size_t i = 0; i < count; i++) {
        if (edges[i].subject == subject && edges[i].relation == REBAC_GRAPH_REL_MEMBER_OF) {
            uint64_t group = edges[i].object;
            if (check_direct_and_member_of(edges, count, group, object, min_relation, fuel - 1)) {
                return true;
            }
        }
    }

    return false;
}

static bool check_delegation(const rebac_edge_t *edges, size_t count,
                              uint64_t subject, uint64_t object, rebac_action_t action)
{
    /* Delegation inversion, direct port of taskweft's own comment in
     * TwReBAC::check_base ("Delegation inversion for CONTROLS"): only
     * .modify (the owner-rank action) is delegable here, matching
     * this project's own CMD_INSTANCE_ASSET "modify: owner only"
     * boundary -- delegation grants owner-equivalent standing, not a
     * lesser rank a claim could already reach some other way. */
    if (action != REBAC_ACTION_MODIFY) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (edges[i].subject == object && edges[i].object == subject &&
            edges[i].relation == REBAC_GRAPH_REL_DELEGATED_TO) {
            return true;
        }
    }
    return false;
}

bool rebac_check_graph(const rebac_edge_t *edges, size_t count,
                        uint64_t subject, uint64_t object,
                        rebac_action_t action, int fuel)
{
    rebac_relation_t min_relation = rebac_action_min_relation(action);
    if (check_direct_and_member_of(edges, count, subject, object, min_relation, fuel)) {
        return true;
    }
    return check_delegation(edges, count, subject, object, action);
}

bool rebac_check_graph_matches_flat(const rebac_relation_t *relations, size_t count,
                                     rebac_action_t action)
{
    /* One object (id 1), one subject (id 0), one edge per relation
     * the claim holds -- the exact one-claim, one-object shape
     * rebac_check() itself evaluates, so the two functions are
     * comparable at all. */
    rebac_edge_t edges[32];
    if (count > 32) {
        count = 32; /* test-only helper; callers keep well under this */
    }
    for (size_t i = 0; i < count; i++) {
        edges[i].subject = 0;
        edges[i].object = 1;
        edges[i].relation = (rebac_graph_rel_t)relations[i];
    }
    bool flat = rebac_check(relations, count, action);
    bool graph = rebac_check_graph(edges, count, 0, 1, action, REBAC_GRAPH_DEFAULT_FUEL);
    return flat == graph;
}
