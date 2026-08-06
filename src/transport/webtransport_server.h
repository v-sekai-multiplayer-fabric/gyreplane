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

/* CORRECTED: a zone fabric is multiple processes, each handling exactly
 * one zone (1 process : 1 zone), not one process ticking a fixed array
 * of zones internally. The earlier WT_SERVER_ZONE_FABRIC_SIZE=4 design
 * (N zones ticked in a loop inside a single process) was wrong -- it
 * does not match zone-server/AGENTS.md's actual deployment shape (one
 * UDP port per zone instance, up to 100 concurrent zones, i.e. up to
 * 100 concurrent *processes*), and it does not match RFD 0002's
 * core-scaling argument either: "each core processes independent
 * zones" describes independent processes/cores, not one process
 * internally looping over several zones. This process now handles
 * exactly one zone, whose z_id is supplied at startup (see main.c's
 * -z<zone_id> flag). Coordinating many such processes/zones is
 * `docs/0001-defer-nogod-gossip-authority.md`'s gossip/VClock question
 * -- still real work, not yet done, but now correctly scoped as
 * "multiple processes, multiple zones, 1-1" instead of folded into a
 * single-process loop that never needed it. */

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

    /* This process handles exactly one zone -- z_id, set at startup.
     * zone_in_flight guards that single zone's FDB transaction so a
     * second ZoneTick never starts before the previous one commits. */
    uint32_t z_id;
    bool zone_in_flight;

    /* H3/WebTransport session context (task #12) -- owns the path table
     * routing ZONE_WT_PATH to the session callbacks in wt_session.c. */
    h3zero_callback_ctx_t *wt_ctx;

    zonetick_entity_t entities[ZONETICK_MAX_ENTITIES];
} webtransport_server_t;

/* Binds UDP `port`, creates the picoquic context (cert_file/key_file are
 * PEM paths, matching zone-server's TLS_CERT/TLS_KEY per zone-server/AGENTS.md),
 * and wires both the UDP socket and a timerfd into `loop`. z_id is the
 * one zone this process handles (see the "fabric = multiple processes"
 * correction above). Returns 0 on success. */
int webtransport_server_init(webtransport_server_t *server, h2o_loop_t *loop,
                              int port, const char *cert_file, const char *key_file,
                              fdb_thread_state_t *fdb_state, uint32_t z_id);

void webtransport_server_close(webtransport_server_t *server);
