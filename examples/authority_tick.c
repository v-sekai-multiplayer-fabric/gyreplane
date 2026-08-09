/*
 * authority_tick.c -- single-writer zone authority on the h2o event loop.
 *
 * One process owns one zone (Pegboard's single-writer invariant). The h2o loop
 * drives a fixed 20 Hz tick; each tick is one read -> integer update -> write
 * -> commit.
 *
 * The actor has NO database. A Rivet actor never opens FDB, postgres, or a
 * file: it reaches its own state back through the engine over the Rivet API.
 * Here that is the Actor Runtime Socket -- SQLite over a Unix domain socket at
 * $ACTOR_RUNTIME_SOCKET_PATH, length-prefixed vbare frames, transactions keyed
 * by a client-generated leaseKey. The engine behind that socket is what
 * abstracts filesystem/postgres/fdb; the actor only ever speaks the Rivet API.
 *
 * Mutations proposed by clients are gated by rebac before they reach state:
 * authority is not just "one writer", it is "one writer that checks".
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <h2o.h>

#include "gen/rebac.h"
#include "gen/xr_grid_entity_packet.h" /* zf_entity_val_t == xr_grid_entity_packet_t */

/* ---- Rivet state port: the Rivet API, not a database --------------------
 *
 * `conn` is an open Actor Runtime Socket connection. An adapter implements
 * these three calls as SQLite requests over that socket:
 *   read   -> BEGIN with a fresh leaseKey, SELECT the zone's entity rows;
 *   write  -> UPDATE those rows with the same leaseKey;
 *   commit -> COMMIT the leaseKey transaction.
 * Encode the frames with the codec generated from
 * engine/sdks/rust/actor-runtime-socket-protocol/schemas/v1.bare -- do not
 * hand-roll the wire. Nothing here knows or cares which backend the engine
 * uses underneath.
 */
typedef struct {
	int (*read_entities)(void *conn, uint32_t z_id,
			     xr_grid_entity_packet_t *out, size_t cap, size_t *n);
	int (*write_entities)(void *conn, uint32_t z_id,
			      const xr_grid_entity_packet_t *ents, size_t n);
	int (*commit)(void *conn);
	void *conn;
} rivet_state_t;

/* ---- the tick: pure, integer, per-tick -- no floats, no dt --------------- */

/*
 * Wire velocity is already a per-tick displacement scaled to i16, so the
 * update is integer and matches zf_zonetick.h exactly:
 *   pos_um += (int64_t)vel_i16 * V_MAX_UM_PER_TICK / INT16_MAX
 */
static void integrate_one_tick(xr_grid_entity_packet_t *e)
{
	const int64_t vmax = XR_PACKET_V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK;
	e->pos_um_x += (int64_t)e->vel_x * vmax / INT16_MAX;
	e->pos_um_y += (int64_t)e->vel_y * vmax / INT16_MAX;
	e->pos_um_z += (int64_t)e->vel_z * vmax / INT16_MAX;
	e->hlc += 1; /* advance the frame counter; see lean-entity-packet hlc */
}

#define ZONE_MAX_ENTITIES 4096

/* Runs one authoritative tick. Returns 0 on success. Safe to call again only
 * after this returns, so at most one leaseKey transaction is ever in flight. */
int authority_tick(const rivet_state_t *state, uint32_t z_id)
{
	static xr_grid_entity_packet_t ents[ZONE_MAX_ENTITIES];
	size_t n = 0;
	if (state->read_entities(state->conn, z_id, ents, ZONE_MAX_ENTITIES, &n) != 0)
		return -1;
	for (size_t i = 0; i < n; i++)
		integrate_one_tick(&ents[i]);
	if (state->write_entities(state->conn, z_id, ents, n) != 0)
		return -1;
	return state->commit(state->conn);
}

/* ---- client mutation, gated by rebac before it reaches state ------------- */

/*
 * A client-proposed change (teleport, spawn, pick-up) is refused unless the
 * caller's relations satisfy the action's minimum. This is the "checks" half
 * of single-writer authority.
 */
bool authority_allow_mutation(const rebac_relation_t *caller_relations,
			      size_t count, rebac_action_t action)
{
	return rebac_check(caller_relations, count, action);
}

/* ---- driving the tick from the h2o loop ---------------------------------- */

typedef struct {
	h2o_timer_t timer;   /* rearmed every tick */
	h2o_loop_t *loop;
	rivet_state_t state;
	uint32_t z_id;
} authority_ctx_t;

#define TICK_INTERVAL_MS (1000 / PBVH_SIM_TICK_HZ) /* 20 Hz -> 50 ms */

static void on_tick(h2o_timer_t *t)
{
	authority_ctx_t *ctx = H2O_STRUCT_FROM_MEMBER(authority_ctx_t, timer, t);
	if (authority_tick(&ctx->state, ctx->z_id) != 0)
		h2o_error_printf("authority_tick: zone %u tick failed\n", ctx->z_id);
	/* Rearm on the same loop thread the socket connection lives on. */
	h2o_timer_link(ctx->loop, TICK_INTERVAL_MS, &ctx->timer);
}

/* Call once after the h2o context is up and the runtime socket is connected
 * (see src/main.c for the loop bootstrap). */
void authority_start(authority_ctx_t *ctx, h2o_loop_t *loop,
		     rivet_state_t state, uint32_t z_id)
{
	ctx->loop = loop;
	ctx->state = state;
	ctx->z_id = z_id;
	ctx->timer.cb = on_tick;
	h2o_timer_link(loop, TICK_INTERVAL_MS, &ctx->timer);
}
