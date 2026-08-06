#pragma once

#include <h2o.h>
#include <picoquic.h>
#include <h3zero_common.h>
#include <stdint.h>

#include "../fdb_database.h"

/* Fixed-size in-memory entity table for the bare ZoneTick (task #11).
 * No FDB, no physics -- position += velocity * dt only. FDB-backed
 * persistence is task #7 (zonefabric M3); this struct is deliberately
 * throwaway, not the eventual zf_entity shape (that comes from
 * lean-entity-packet codegen, task #10). */
#define ZONETICK_MAX_ENTITIES 256

/* How many zones this one process ticks per event -- a small "fabric,"
 * per direct correction that a zone is a fabric of zones, not a
 * singleton. Each zone gets its own FDB transaction
 * (zf_zonetick_run in zf_zonetick.c), with no cross-zone lock or shared
 * state -- RFD 0002's own core-scaling argument ("no cross-zone
 * conflicts... near-linear core scaling, unlike TPC-C where
 * district-level conflicts cause retries") depends on exactly that
 * independence, and this fabric size is kept small and compile-time
 * fixed so the property is easy to keep true by inspection rather than
 * proven only by a benchmark. Actually measuring linear scaling of
 * concurrent zone ticks needs a running FDB cluster and load generator
 * (RFD 0013's wrk harness) -- not something this sandbox can execute;
 * flagged as a real gap, not silently assumed passing. */
#define WT_SERVER_ZONE_FABRIC_SIZE 4

typedef struct {
    int active;
    double cx, cy, cz;
    double vx, vy, vz;
} zonetick_entity_t;

typedef struct {
    picoquic_quic_t *quic;

    int udp_fd;
    h2o_socket_t *udp_sock;

    int timer_fd;
    h2o_socket_t *timer_sock;

    h2o_loop_t *loop;
    int port;

    /* Task #7: real FDB-backed ZoneTick (zf_zonetick.c) supersedes the
     * in-memory-only `entities` table below for anything backed by this
     * fdb_state. */
    fdb_thread_state_t *fdb_state;

    /* This process handles a fabric of WT_SERVER_ZONE_FABRIC_SIZE zones
     * (z_id 0..N-1), not a single hardcoded zone -- see
     * zonetick_fdb_all_zones() in webtransport_server.c for why that
     * changed and what "fabric" does/doesn't mean here. One in-flight
     * guard per zone so no zone's pending commit blocks another's tick. */
    bool zone_in_flight[WT_SERVER_ZONE_FABRIC_SIZE];

    /* H3/WebTransport session context (task #12) -- owns the path table
     * routing ZONE_WT_PATH to the session callbacks in wt_session.c. */
    h3zero_callback_ctx_t *wt_ctx;

    zonetick_entity_t entities[ZONETICK_MAX_ENTITIES];
} webtransport_server_t;

/* Binds UDP `port`, creates the picoquic context (cert_file/key_file are
 * PEM paths, matching zone-server's TLS_CERT/TLS_KEY per zone-server/AGENTS.md),
 * and wires both the UDP socket and a timerfd into `loop`. Returns 0 on
 * success. */
int webtransport_server_init(webtransport_server_t *server, h2o_loop_t *loop,
                              int port, const char *cert_file, const char *key_file,
                              fdb_thread_state_t *fdb_state);

void webtransport_server_close(webtransport_server_t *server);
