#pragma once

#include <h2o.h>
#include <picoquic.h>
#include <stdint.h>

/* Fixed-size in-memory entity table for the bare ZoneTick (task #11).
 * No FDB, no physics -- position += velocity * dt only. FDB-backed
 * persistence is task #7 (zonefabric M3); this struct is deliberately
 * throwaway, not the eventual zf_entity shape (that comes from
 * lean-entity-packet codegen, task #10). */
#define ZONETICK_MAX_ENTITIES 256

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

    zonetick_entity_t entities[ZONETICK_MAX_ENTITIES];
} webtransport_server_t;

/* Binds UDP `port`, creates the picoquic context (cert_file/key_file are
 * PEM paths, matching zone-server's TLS_CERT/TLS_KEY per zone-server/AGENTS.md),
 * and wires both the UDP socket and a timerfd into `loop`. Returns 0 on
 * success. */
int webtransport_server_init(webtransport_server_t *server, h2o_loop_t *loop,
                              int port, const char *cert_file, const char *key_file);

void webtransport_server_close(webtransport_server_t *server);
