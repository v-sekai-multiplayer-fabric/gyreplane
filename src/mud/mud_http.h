#ifndef MUD_HTTP_H_
#define MUD_HTTP_H_

/*
 * mud_http: the MUD prototype's h2o-facing surface. Registers real
 * paths under the same h2o_hostconf_t src/main.c's own
 * h2o_config_register_host() call already returns -- no separate
 * "full libh2o" link is needed, h2o_create_handler()/h2o_file_register()
 * are both already exported by the h2o-evloop library this project
 * already links (confirmed via `nm` against the real built archive
 * before writing this file).
 */

#include <h2o.h>

#include "../fdb_database.h"

/* docroot: real filesystem path to mud/web/ (the static site).
 * orchestrator_path/guest_elf_path: passed straight through to
 * mud_session_init(). Registers "/", "/api/mud/command",
 * "/api/mud/stream", and "/api/mud/history" under hostconf. */
void mud_http_register(h2o_hostconf_t *hostconf, const char *docroot, const char *orchestrator_path, const char *guest_elf_path);

/* Real FDB durability, not a placeholder: once set, on_mud_command()
 * writes each turn's own narration to zf/mud/turn/{session_id}/{turn}
 * (mud_kv.h) after every real mud_step() result, and
 * GET /api/mud/history?session_id=... range-reads them back as a real
 * JSON array. Call once, before any request can arrive -- matching
 * mud_http_listen()'s own thread-0-only, called-once contract. A mud
 * process with no FDB state set (this function never called) simply
 * skips the write/read, matching every other optional piece of this
 * feature (TLS, the Tigris backup). */
void mud_http_set_fdb_state(fdb_thread_state_t *state);

/* Binds a real TCP listener on `port` and wires it into `loop` via
 * h2o_evloop_socket_create()/h2o_accept(), the same pattern
 * examples/libh2o/simple.c uses -- mud_http_register() alone only
 * registers paths, nothing accepts a connection without this. If
 * cert_file/key_file are non-NULL, serves HTTPS (with HTTP/2 ALPN);
 * otherwise plain HTTP. Call once, thread 0 only (matching
 * webtransport_server_init()'s own single-thread-0-listener rule).
 * Returns 0 on success, -1 on a real bind/listen/TLS-setup failure. */
int mud_http_listen(h2o_context_t *ctx, h2o_loop_t *loop, int port, const char *cert_file, const char *key_file);

/* Real per-tick safety-net flush for connected /api/mud/stream SSE
 * clients -- not a per-tick send to every client (see mud_http.c's own
 * block comment on the SSE registry for why). Call from
 * on_zonetick_timer_fire() (webtransport_server.c), the existing
 * ZONE_TICK_HZ driver, every tick. A no-op when nothing is queued,
 * which is the common case since sse_push() already sends immediately
 * whenever a client is ready. */
void mud_http_flush_streams(void);

#endif // MUD_HTTP_H_
