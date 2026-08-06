/*
 * WebTransport/QUIC datagram server, built on the vendored picoquic +
 * picotls (cmake/picoquic.cmake), matching the Godot client's
 * WebTransportPeer backend (quic_picoquic_backend.cpp in the Godot fork).
 *
 * Status: not yet implemented -- this is the wiring point for task #11
 * (transport + basic ZoneTick, the plan's first working slice). picoquic's
 * server-side API (picoquic_create, picoquic_set_default_callback, the
 * H3/WebTransport callback registration in picohttp/webtransport.c) needs
 * to be driven from h2o's event loop (src/event_loop.c) the same way
 * fdb_database.c drives FDB's async callbacks -- picoquic is UDP/socket-based,
 * not naturally epoll-integrated with h2o-evloop, so that integration is
 * the actual first-slice work, not a formality.
 */

#include "webtransport_server.h"

int webtransport_server_init(webtransport_server_t *server, int port)
{
    (void)server;
    (void)port;
    /* TODO(task #11): picoquic_create() + register the WebTransport H3
     * callback (see thirdparty/picoquic/picohttp/webtransport.c and
     * wt_baton.c for the reference shape), bridge its socket fd into
     * src/event_loop.c's h2o_evloop via h2o_evloop_socket_create(). */
    return -1;
}
