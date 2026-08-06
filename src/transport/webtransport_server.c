/*
 * WebTransport/QUIC datagram server, bridging vendored picoquic
 * (thirdparty/picoquic, cmake/picoquic.cmake) into h2o's evloop
 * (src/event_loop.c), per task #11 (plan step 0: transport + basic
 * ZoneTick). Originally an in-memory-only placeholder; task #7 wired in
 * the real FDB-backed ZoneTick (zf_zonetick.c) once fdb_state is set --
 * see zonetick_fdb_all_zones() below, which ticks a small fabric of
 * zones, not a single hardcoded one (see that function's own comment).
 * Task #12 wired in the real H3/WebTransport
 * session layer (wt_session.c) -- see that file's header for the exact
 * call sequence, grounded against picoquic's own reference server. Still
 * no physics/IK (task #8).
 *
 * picoquic owns no event loop of its own here -- picoquic_packet_loop()
 * (picoquic's built-in blocking loop) is NOT used, because it would fight
 * h2o_evloop_run() for the thread. Instead this drives picoquic's
 * "manual" API directly:
 *   - picoquic_incoming_packet()     feed a received UDP datagram in
 *   - picoquic_prepare_next_packet() pull the next outbound packet out
 *   - picoquic_get_next_wake_time()  when to call prepare_next_packet
 *                                    again even with no incoming traffic
 *                                    (retransmits, ACKs, idle timers)
 * The wake timer is a POSIX timerfd (Linux-only, matches h2o-evloop's
 * existing Linux-only assumptions in this repo) wrapped in its own
 * h2o_socket_t, since h2o's own timer API was not confirmed against the
 * exact vendored h2o commit and a wrong guess here is worse than a
 * well-understood POSIX primitive.
 *
 * STATUS: real WebTransport sessions on ZONE_WT_PATH (wt_session.h) now
 * drive the ZoneTick, not raw QUIC datagrams on any connection -- see
 * wt_session.c's header for what's grounded against picoquic's reference
 * server versus what's flagged unverified (protocol-string negotiation,
 * TLS cert/key are still NULL/NULL below).
 */

#include "webtransport_server.h"
#include "wt_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "../zf_zonetick.h"

#define ZONETICK_RECV_BUF 2048
#define ZONETICK_SEND_BUF 2048

static void flush_outbound(webtransport_server_t *server)
{
    uint8_t send_buffer[ZONETICK_SEND_BUF];
    size_t send_length = 0;
    struct sockaddr_storage addr_to, addr_from;
    int if_index = 0;
    picoquic_connection_id_t log_cid;
    picoquic_cnx_t *last_cnx = NULL;
    uint64_t current_time = picoquic_current_time();

    for (;;) {
        int ret = picoquic_prepare_next_packet(server->quic, current_time,
            send_buffer, sizeof(send_buffer), &send_length,
            &addr_to, &addr_from, &if_index, &log_cid, &last_cnx);

        if (ret != 0 || send_length == 0) {
            break;
        }

        (void)sendto(server->udp_fd, send_buffer, send_length, 0,
            (struct sockaddr *)&addr_to,
            addr_to.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    }

    /* Rearm the wake timer for whenever picoquic next needs a tick, even
     * with no new incoming packets (retransmit/idle timers, etc). */
    uint64_t next_wake = picoquic_get_next_wake_time(server->quic, current_time);
    int64_t delta_us = (int64_t)(next_wake - current_time);
    if (delta_us < 1000) {
        delta_us = 1000; /* floor at 1ms so a bad/negative delta can't spin the loop */
    }

    struct itimerspec its = {0};
    its.it_value.tv_sec = delta_us / 1000000;
    its.it_value.tv_nsec = (delta_us % 1000000) * 1000;
    timerfd_settime(server->timer_fd, 0, &its, NULL);
}

/* Task #11's original in-memory-only placeholder. Superseded by
 * zf_zonetick_run() below for anything with fdb_state set, but left in
 * place (unused when fdb_state != NULL) as a no-FDB fallback / reference
 * for what the FDB version replaced. */
static void zonetick_step(webtransport_server_t *server, double dt)
{
    for (int i = 0; i < ZONETICK_MAX_ENTITIES; i++) {
        zonetick_entity_t *e = &server->entities[i];
        if (!e->active) {
            continue;
        }
        e->cx += e->vx * dt;
        e->cy += e->vy * dt;
        e->cz += e->vz * dt;
    }
}

typedef struct {
    webtransport_server_t *server;
    uint32_t z_id;
} zonetick_done_ctx_t;

static void on_zonetick_done(void *ctx, bool ok)
{
    zonetick_done_ctx_t *dctx = (zonetick_done_ctx_t *)ctx;
    dctx->server->zone_in_flight[dctx->z_id] = false;
    if (!ok) {
        fprintf(stderr, "webtransport_server: zone %u ZoneTick failed (unretryable FDB error)\n",
                dctx->z_id);
    }
    free(dctx);
}

/* Task #7 (M3) + this pass's correction: z_id was hardcoded to 0 --
 * "one process, one zone" -- until told directly that a zone is a fabric
 * of zones and needs testing as such. This process now ticks
 * WT_SERVER_ZONE_FABRIC_SIZE zones (z_id 0..N-1) every datagram/timer
 * event, each with its own in-flight guard (zone_in_flight[]) so one
 * zone's pending commit never blocks another's tick -- the isolation
 * this relies on (no two zones' FDB key ranges ever overlap) is proven
 * in test/unit/test_zf_kv_multi_zone.c, not just asserted here.
 *
 * What this still is NOT: multiple zone-server *processes* coordinating
 * over a network (that needs NoGod.lean's gossip protocol, per
 * docs/0001-defer-nogod-gossip-authority.md -- still correctly deferred,
 * a separate question from "does this one process handle more than one
 * zone correctly"). tick_count is 1 -- task #14 moved zf_zonetick_run()
 * from a float dt to an integer tick_count (see zf_zonetick.h's header
 * comment for why: wire velocity is already a per-tick displacement). */
static void zonetick_fdb_all_zones(webtransport_server_t *server)
{
    for (uint32_t z_id = 0; z_id < WT_SERVER_ZONE_FABRIC_SIZE; z_id++) {
        if (server->zone_in_flight[z_id]) {
            continue; /* this zone's previous tick hasn't committed yet */
        }
        zonetick_done_ctx_t *dctx = calloc(1, sizeof(*dctx));
        if (dctx == NULL) {
            continue;
        }
        dctx->server = server;
        dctx->z_id = z_id;
        server->zone_in_flight[z_id] = true;
        if (zf_zonetick_run(server->fdb_state, z_id, /* tick_count */ 1,
                             on_zonetick_done, dctx) != 0) {
            server->zone_in_flight[z_id] = false;
            free(dctx);
        }
    }
}

/* Fired by wt_session.c's zone_wt_session_callback on
 * picohttp_callback_post_datagram -- i.e. a real negotiated WebTransport
 * session datagram on ZONE_WT_PATH, not a raw QUIC datagram on any
 * connection (that was task #11's placeholder scope; task #12 replaced
 * it). One fixed tick across the whole zone fabric per datagram
 * (matches godot-loop-slice's TICK_HZ=30 cadence assumption). The
 * in-memory zonetick_step() fallback is used only when fdb_state is
 * NULL (still float-dt based; never migrated off that shape since the
 * FDB path superseded it before that would have mattered). */
static void on_wt_datagram(void *app_ctx)
{
    webtransport_server_t *server = (webtransport_server_t *)app_ctx;
    if (server->fdb_state != NULL) {
        zonetick_fdb_all_zones(server);
    } else {
        zonetick_step(server, 1.0 / 30.0);
    }
}

static void on_udp_readable(h2o_socket_t *sock, const char *err)
{
    webtransport_server_t *server = (webtransport_server_t *)sock->data;
    uint8_t recv_buffer[ZONETICK_RECV_BUF];
    struct sockaddr_storage peer_addr;
    struct sockaddr_storage local_addr;
    socklen_t peer_len;

    if (err != NULL) {
        return;
    }

    for (;;) {
        peer_len = sizeof(peer_addr);
        ssize_t n = recvfrom(server->udp_fd, recv_buffer, sizeof(recv_buffer), 0,
            (struct sockaddr *)&peer_addr, &peer_len);
        if (n <= 0) {
            break; /* EAGAIN or error -- drained for this readiness event */
        }

        memset(&local_addr, 0, sizeof(local_addr));
        ((struct sockaddr_in *)&local_addr)->sin_family = AF_INET;
        ((struct sockaddr_in *)&local_addr)->sin_port = htons((uint16_t)server->port);

        picoquic_incoming_packet(server->quic, recv_buffer, (size_t)n,
            (struct sockaddr *)&peer_addr, (struct sockaddr *)&local_addr,
            0, 0, picoquic_current_time());
    }

    flush_outbound(server);
}

static void on_timer_fire(h2o_socket_t *sock, const char *err)
{
    webtransport_server_t *server = (webtransport_server_t *)sock->data;
    uint64_t expirations;

    if (err != NULL) {
        return;
    }

    /* Must read a timerfd on wake or it stays readable forever. */
    (void)read(server->timer_fd, &expirations, sizeof(expirations));
    flush_outbound(server);
}

static int create_udp_socket(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int webtransport_server_init(webtransport_server_t *server, h2o_loop_t *loop,
                              int port, const char *cert_file, const char *key_file,
                              fdb_thread_state_t *fdb_state)
{
    memset(server, 0, sizeof(*server));
    server->loop = loop;
    server->port = port;
    server->fdb_state = fdb_state;

    server->udp_fd = create_udp_socket(port);
    if (server->udp_fd < 0) {
        fprintf(stderr, "webtransport_server: failed to bind UDP port %d: %s\n",
                port, strerror(errno));
        return -1;
    }

    /* zone_wt_create_context builds the h3zero path table (ZONE_WT_PATH ->
     * wt_session.c's session callbacks); on_wt_datagram is what a
     * negotiated session's datagrams ultimately drive. */
    server->wt_ctx = zone_wt_create_context(on_wt_datagram, server);
    if (server->wt_ctx == NULL) {
        fprintf(stderr, "webtransport_server: zone_wt_create_context failed\n");
        close(server->udp_fd);
        return -1;
    }

    uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = {0};
    server->quic = picoquic_create(
        /* max_nb_connections */ 256,
        cert_file, key_file, /* cert_root_file_name */ NULL,
        /* default_alpn */ "h3", /* fixed -- see wt_session.h's header comment
                                     for why this skips alpn_select_fn entirely */
        h3zero_callback, server->wt_ctx,
        /* cnx_id_callback */ NULL, /* cnx_id_callback_data */ NULL,
        reset_seed,
        picoquic_current_time(), /* current_time */
        NULL, /* p_simulated_time */
        NULL, /* ticket_file_name */
        NULL, 0 /* ticket_encryption_key */);

    if (server->quic == NULL) {
        fprintf(stderr, "webtransport_server: picoquic_create failed\n");
        zone_wt_free_context(NULL, server->wt_ctx);
        close(server->udp_fd);
        return -1;
    }

    server->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (server->timer_fd < 0) {
        fprintf(stderr, "webtransport_server: timerfd_create failed: %s\n", strerror(errno));
        picoquic_free(server->quic);
        close(server->udp_fd);
        return -1;
    }

    server->udp_sock = h2o_evloop_socket_create(loop, server->udp_fd, H2O_SOCKET_FLAG_DONT_READ);
    server->udp_sock->data = server;
    h2o_socket_read_start(server->udp_sock, on_udp_readable);

    server->timer_sock = h2o_evloop_socket_create(loop, server->timer_fd, H2O_SOCKET_FLAG_DONT_READ);
    server->timer_sock->data = server;
    h2o_socket_read_start(server->timer_sock, on_timer_fire);

    fprintf(stderr, "webtransport_server: WebTransport bound on UDP %d, path %s, "
                     "fabric of %d zone(s) (TLS cert/key still NULL/NULL -- unauthenticated)\n",
            port, ZONE_WT_PATH, WT_SERVER_ZONE_FABRIC_SIZE);

    return 0;
}

void webtransport_server_close(webtransport_server_t *server)
{
    if (server->udp_sock != NULL) {
        h2o_socket_read_stop(server->udp_sock);
        h2o_socket_close(server->udp_sock);
    }
    if (server->timer_sock != NULL) {
        h2o_socket_read_stop(server->timer_sock);
        h2o_socket_close(server->timer_sock);
    }
    if (server->quic != NULL) {
        picoquic_free(server->quic);
    }
    zone_wt_free_context(NULL, server->wt_ctx);
    if (server->timer_fd >= 0) {
        close(server->timer_fd);
    }
}
