/*
 * WebTransport/QUIC datagram server, bridging vendored picoquic
 * (thirdparty/picoquic, cmake/picoquic.cmake) into h2o's evloop
 * (src/event_loop.c), per task #11 (plan step 0: transport + basic
 * ZoneTick, no FDB, no physics).
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
 * STATUS: this wires the QUIC transport layer (packets in/out, connection
 * lifecycle) end-to-end. It does NOT yet negotiate WebTransport sessions
 * over HTTP/3 -- that is picohttp/webtransport.c's h3zero server layer
 * (ALPN "h3", CONNECT :protocol=webtransport), not yet registered here.
 * The default callback below fires for raw QUIC stream/datagram events on
 * any connection that completes a handshake; it drives the bare ZoneTick
 * off picoquic_callback_datagram as a placeholder for what will become a
 * real WebTransport datagram once that H3 layer is wired in. Do not read
 * this as "WebTransport works" -- it is "QUIC transport works," one layer
 * short.
 */

#include "webtransport_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

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

/* Placeholder ZoneTick: applies position += velocity * dt to every active
 * entity. No FDB (task #7), no physics/IK (task #8) -- this exists only
 * to prove a datagram can drive a tick end-to-end. */
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

static int default_stream_callback(picoquic_cnx_t *cnx, uint64_t stream_id,
                                    uint8_t *bytes, size_t length,
                                    picoquic_call_back_event_t fin_or_event,
                                    void *callback_ctx, void *stream_ctx)
{
    webtransport_server_t *server = (webtransport_server_t *)callback_ctx;
    (void)cnx;
    (void)stream_id;
    (void)bytes;
    (void)length;
    (void)stream_ctx;

    switch (fin_or_event) {
    case picoquic_callback_datagram:
        /* See file header: this is QUIC-level, not yet a negotiated
         * WebTransport session datagram. Ticking anyway to prove the
         * receive -> tick path, per task #11's stated scope. */
        zonetick_step(server, 1.0 / 30.0 /* placeholder fixed dt, matches
                                             godot-loop-slice's TICK_HZ=30 */);
        break;
    default:
        break;
    }

    return 0;
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
                              int port, const char *cert_file, const char *key_file)
{
    memset(server, 0, sizeof(*server));
    server->loop = loop;
    server->port = port;

    server->udp_fd = create_udp_socket(port);
    if (server->udp_fd < 0) {
        fprintf(stderr, "webtransport_server: failed to bind UDP port %d: %s\n",
                port, strerror(errno));
        return -1;
    }

    uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = {0};
    server->quic = picoquic_create(
        /* max_nb_connections */ 256,
        cert_file, key_file, /* cert_root_file_name */ NULL,
        /* default_alpn */ NULL, /* not "h3" yet -- see file header */
        default_stream_callback, server,
        /* cnx_id_callback */ NULL, /* cnx_id_callback_data */ NULL,
        reset_seed,
        picoquic_current_time(), /* current_time */
        NULL, /* p_simulated_time */
        NULL, /* ticket_file_name */
        NULL, 0 /* ticket_encryption_key */);

    if (server->quic == NULL) {
        fprintf(stderr, "webtransport_server: picoquic_create failed\n");
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

    fprintf(stderr, "webtransport_server: QUIC transport bound on UDP %d "
                     "(WebTransport/H3 session layer not yet wired -- see file header)\n",
            port);

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
    if (server->timer_fd >= 0) {
        close(server->timer_fd);
    }
}
