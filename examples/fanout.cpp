/*
 * fanout.cpp -- interest-filtered fan-out, the leaf of the relay tree.
 *
 * Bandwidth is bounded by one NIC per process, so fan-out cannot broadcast the
 * whole zone to everyone. Two things bound it, and both are here:
 *   - interest: predictive_bvh decides who-sees-whom, so each subscriber gets
 *     only its slice (ghost-AABB overlap in int64-micrometre space);
 *   - the tree: each relay process runs this leaf for its own subscriber set on
 *     its own NIC, so aggregate egress scales with relay count.
 *
 * The wire payload is the codegen'd XRGridEntityPacket, back-to-back: one write
 * per subscriber per tick, count recovered as len / 100. No framing.
 *
 * The entity set comes from the authority tick, which read it back through the
 * Rivet API (the Actor Runtime Socket) -- this leaf never touches storage.
 *
 * C++ because predictive_bvh.h is a C++ header (templated AabbT<int64_t>). The
 * repo already compiles C++ (see src/sandbox/sandbox_guest.cpp).
 */

#include <cstdint>
#include <cstddef>

#include "predictive_bvh.h"
extern "C" {
#include "gen/xr_grid_entity_packet.h"
}

/*
 * Delivery seam. WebTransport is not in the container yet, so a subscriber's
 * bytes go through this function pointer. The default is an ordinary h2o send;
 * swap it for a WebTransport datagram sink later without touching the interest
 * logic below.
 */
typedef void (*fanout_sink_t)(void *subscriber, const uint8_t *buf, size_t len);

struct subscriber_t {
	Aabb interest;    /* the box this subscriber cares about, in micrometres */
	void *conn;       /* opaque delivery handle (an h2o generator, a WT session) */
	fanout_sink_t send;
};

/* Interest tuning, documented so it is not mistaken for magic. */
static const int64_t ENTITY_EXT_UM = 500000;   /* half-extent treated per entity */
static const int64_t ACCEL_HALF_UM = 0;        /* no acceleration ghosting here */
static const int64_t GHOST_TICKS    = 2;       /* expand along velocity, 2 ticks */

/* Build an entity's velocity-ghosted AABB in the same int64-um space the
 * packet's positions already live in, so no unit conversion is needed beyond
 * the i16 -> um/tick develocitising the packet spec defines. */
static Aabb ghost_aabb_of(const xr_grid_entity_packet_t &e)
{
	const int64_t vmax = XR_PACKET_V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK;
	const int64_t vx = (int64_t)e.vel_x * vmax / INT16_MAX;
	const int64_t vy = (int64_t)e.vel_y * vmax / INT16_MAX;
	const int64_t vz = (int64_t)e.vel_z * vmax / INT16_MAX;
	Aabb b;
	b.min_x = ghost_aabb_min(e.pos_um_x, ENTITY_EXT_UM, vx, ACCEL_HALF_UM, GHOST_TICKS);
	b.max_x = ghost_aabb_max(e.pos_um_x, ENTITY_EXT_UM, vx, ACCEL_HALF_UM, GHOST_TICKS);
	b.min_y = ghost_aabb_min(e.pos_um_y, ENTITY_EXT_UM, vy, ACCEL_HALF_UM, GHOST_TICKS);
	b.max_y = ghost_aabb_max(e.pos_um_y, ENTITY_EXT_UM, vy, ACCEL_HALF_UM, GHOST_TICKS);
	b.min_z = ghost_aabb_min(e.pos_um_z, ENTITY_EXT_UM, vz, ACCEL_HALF_UM, GHOST_TICKS);
	b.max_z = ghost_aabb_max(e.pos_um_z, ENTITY_EXT_UM, vz, ACCEL_HALF_UM, GHOST_TICKS);
	return b;
}

/* Cap one subscriber's slice so a single write stays inside a datagram-sized
 * batch. 64 entities * 100 bytes = 6400 bytes, comfortably inside one message. */
#define MAX_SLICE_ENTITIES 64

/*
 * Fan one tick of the zone out to one subscriber: filter by interest, pack the
 * survivors back-to-back, send once. Returns the number of entities sent.
 */
size_t fanout_one(const subscriber_t &sub,
		  const xr_grid_entity_packet_t *ents, size_t n)
{
	static uint8_t batch[MAX_SLICE_ENTITIES * XR_PACKET_SIZE];
	size_t out = 0;
	for (size_t i = 0; i < n && out < MAX_SLICE_ENTITIES; i++) {
		Aabb g = ghost_aabb_of(ents[i]);
		if (!aabb_overlaps(&g, &sub.interest))
			continue; /* not in this subscriber's interest */
		xr_grid_entity_packet_encode(&ents[i], batch + out * XR_PACKET_SIZE);
		out++;
	}
	if (out > 0)
		sub.send(sub.conn, batch, out * XR_PACKET_SIZE);
	return out;
}

/* Fan the whole zone out to every subscriber of this relay leaf. Called once
 * per tick, after the authority tick has produced the current entity set. */
void fanout_tick(const subscriber_t *subs, size_t sub_count,
		 const xr_grid_entity_packet_t *ents, size_t n)
{
	for (size_t s = 0; s < sub_count; s++)
		fanout_one(subs[s], ents, n);
}
