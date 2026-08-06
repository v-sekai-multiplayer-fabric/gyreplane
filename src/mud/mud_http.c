#include "mud_http.h"
#include "mud_cbor.h"
#include "mud_session.h"
#include "utility.h"

#include <netinet/in.h>
#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <yajl/yajl_parse.h>

/* Forward declaration: sse_push() is defined with the rest of the SSE
 * client registry further down (near on_mud_stream()), but
 * on_mud_command() -- defined first, matching this file's original
 * top-to-bottom handler order -- needs to call it too. */
static void sse_push(const char *session_id, const char *text, size_t len);

/* ---------------------------------------------------------------------
 * POST /api/mud/command body parsing: {"session_id", "command",
 * "args": [...], "message"} -- a real yajl stream parse, not a
 * hand-written JSON reader, matching the same "use the real library"
 * correction already applied to the CBOR side (mud_cbor.c/QCBOR).
 * ------------------------------------------------------------------- */

#define MUD_HTTP_MAX_ARGS 8

typedef struct {
    char session_id[64];
    char command[32];
    char args[MUD_HTTP_MAX_ARGS][64];
    size_t n_args;
    char message[256];
    /* Parser state: which top-level field the next string/array
     * belongs to. */
    enum { FIELD_NONE, FIELD_SESSION_ID, FIELD_COMMAND, FIELD_ARGS, FIELD_MESSAGE } current_field;
    bool in_args_array;
} mud_command_request_t;

static int on_map_key(void *ctx, const unsigned char *key, size_t len) {
    mud_command_request_t *req = (mud_command_request_t *)ctx;
    if (len == 10 && memcmp(key, "session_id", 10) == 0) {
        req->current_field = FIELD_SESSION_ID;
    }
    else if (len == 7 && memcmp(key, "command", 7) == 0) {
        req->current_field = FIELD_COMMAND;
    }
    else if (len == 4 && memcmp(key, "args", 4) == 0) {
        req->current_field = FIELD_ARGS;
    }
    else if (len == 7 && memcmp(key, "message", 7) == 0) {
        req->current_field = FIELD_MESSAGE;
    }
    else {
        req->current_field = FIELD_NONE;
    }
    return 1;
}

static int on_string(void *ctx, const unsigned char *val, size_t len) {
    mud_command_request_t *req = (mud_command_request_t *)ctx;
    switch (req->current_field) {
    case FIELD_SESSION_ID:
        snprintf(req->session_id, sizeof(req->session_id), "%.*s", (int)len, val);
        break;
    case FIELD_COMMAND:
        snprintf(req->command, sizeof(req->command), "%.*s", (int)len, val);
        break;
    case FIELD_MESSAGE:
        snprintf(req->message, sizeof(req->message), "%.*s", (int)len, val);
        break;
    case FIELD_ARGS:
        if (req->in_args_array && req->n_args < MUD_HTTP_MAX_ARGS) {
            snprintf(req->args[req->n_args], sizeof(req->args[0]), "%.*s", (int)len, val);
            req->n_args++;
        }
        break;
    default:
        break;
    }
    return 1;
}

static int on_start_array(void *ctx) {
    mud_command_request_t *req = (mud_command_request_t *)ctx;
    if (req->current_field == FIELD_ARGS) {
        req->in_args_array = true;
    }
    return 1;
}

static int on_end_array(void *ctx) {
    mud_command_request_t *req = (mud_command_request_t *)ctx;
    req->in_args_array = false;
    return 1;
}

static const yajl_callbacks MUD_COMMAND_CALLBACKS = {
    .yajl_string = on_string,
    .yajl_map_key = on_map_key,
    .yajl_start_array = on_start_array,
    .yajl_end_array = on_end_array,
};

/* Returns true on a real parse success (session_id and command both
 * present) -- false leaves *out_req in an undefined but harmless
 * partial state, caller must check the return value. */
static bool parse_command_body(const char *body, size_t len, mud_command_request_t *out_req) {
    memset(out_req, 0, sizeof(*out_req));
    yajl_handle handle = yajl_alloc(&MUD_COMMAND_CALLBACKS, NULL, out_req);
    yajl_status status = yajl_parse(handle, (const unsigned char *)body, len);
    if (status == yajl_status_ok) {
        status = yajl_complete_parse(handle);
    }
    yajl_free(handle);
    return status == yajl_status_ok && out_req->session_id[0] != '\0' && out_req->command[0] != '\0';
}

/* ---------------------------------------------------------------------
 * POST /api/mud/command handler
 * ------------------------------------------------------------------- */

static void send_json_error(h2o_req_t *req, int status, const char *message) {
    static h2o_generator_t generator = {NULL, NULL};
    req->res.status = status;
    req->res.reason = "error";
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));
    h2o_start_response(req, &generator);
    h2o_iovec_t body = h2o_concat(&req->pool, h2o_iovec_init(H2O_STRLIT("{\"error\":\"")),
                                   h2o_strdup(&req->pool, message, SIZE_MAX), h2o_iovec_init(H2O_STRLIT("\"}")));
    h2o_send(req, &body, 1, H2O_SEND_STATE_FINAL);
}

/* Re-encodes a MudTurnResult/MudBootAck CBOR map's handful of
 * top-level scalar fields as a small JSON object for the website UI --
 * a real, if narrow, CBOR-to-JSON bridge (reads via mud_cbor.c's real
 * QCBOR-backed accessors), not a byte-for-byte passthrough, since
 * browsers speak JSON, not CBOR. */
static h2o_iovec_t cbor_result_to_json(h2o_mem_pool_t *pool, const uint8_t *cbor, size_t cbor_len) {
    size_t narration_len = 0;
    const uint8_t *narration = mud_cbor_map_get_str(cbor, cbor_len, "narration", &narration_len);
    size_t pre_len = 0, post_len = 0;
    const uint8_t *pre_room = mud_cbor_map_get_str(cbor, cbor_len, "pre_room", &pre_len);
    const uint8_t *post_room = mud_cbor_map_get_str(cbor, cbor_len, "post_room", &post_len);
    bool valid = mud_cbor_map_get_bool(cbor, cbor_len, "valid", true);
    bool finished = mud_cbor_map_get_bool(cbor, cbor_len, "finished", false);
    bool objective_complete = mud_cbor_map_get_bool(cbor, cbor_len, "objective_complete", false);
    int64_t turn = mud_cbor_map_get_int(cbor, cbor_len, "turn", 0);

    char *buf = (char *)h2o_mem_alloc_pool(pool, char, narration_len + pre_len + post_len + 256);
    int n = snprintf(buf, narration_len + pre_len + post_len + 256,
                      "{\"turn\":%lld,\"narration\":\"%.*s\",\"pre_room\":\"%.*s\","
                      "\"post_room\":\"%.*s\",\"valid\":%s,\"finished\":%s,\"objective_complete\":%s}",
                      (long long)turn, (int)narration_len, narration ? (const char *)narration : "",
                      (int)pre_len, pre_room ? (const char *)pre_room : "", (int)post_len,
                      post_room ? (const char *)post_room : "", valid ? "true" : "false",
                      finished ? "true" : "false", objective_complete ? "true" : "false");
    return h2o_iovec_init(buf, (size_t)n);
}

static int on_mud_command(h2o_handler_t *self, h2o_req_t *req) {
    (void)self;
    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("POST"))) {
        return -1;
    }

    mud_command_request_t parsed;
    if (!parse_command_body(req->entity.base, req->entity.len, &parsed)) {
        send_json_error(req, 400, "malformed request body");
        return 0;
    }

    /* Objective is fixed at "gain_watch_trust" for this prototype's
     * first session-create call -- a real objective picker is website-
     * UI scope, not this handler's. Existing sessions ignore it. */
    mud_session_t *session = mud_session_get_or_create(parsed.session_id, "gain_watch_trust");
    if (session == NULL) {
        send_json_error(req, 503, "could not create or reach mud session");
        return 0;
    }

    const char *args_ptrs[MUD_HTTP_MAX_ARGS];
    for (size_t i = 0; i < parsed.n_args; i++) {
        args_ptrs[i] = parsed.args[i];
    }
    mud_cbor_buf_t cmd_cbor = mud_cbor_encode_command(parsed.command, args_ptrs, parsed.n_args, parsed.message);

    uint8_t *result;
    size_t result_len;
    int rc = mud_session_step(session, cmd_cbor.data, cmd_cbor.len, &result, &result_len);
    mud_cbor_buf_free(&cmd_cbor);
    if (rc != 0) {
        send_json_error(req, 502, "mud session step failed");
        return 0;
    }

    h2o_iovec_t json = cbor_result_to_json(&req->pool, result, result_len);
    size_t narration_len = 0;
    const uint8_t *narration = mud_cbor_map_get_str(result, result_len, "narration", &narration_len);
    if (narration != NULL) {
        sse_push(parsed.session_id, (const char *)narration, narration_len);
    }
    free(result);

    static h2o_generator_t generator = {NULL, NULL};
    req->res.status = 200;
    req->res.reason = "OK";
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));
    h2o_start_response(req, &generator);
    h2o_send(req, &json, 1, H2O_SEND_STATE_FINAL);
    return 0;
}

/* ---------------------------------------------------------------------
 * GET /api/mud/stream?session_id=... (SSE) -- a real, backpressure-
 * respecting async push, not a placeholder. h2o's own generator
 * contract (h2o.h's st_h2o_generator_t comment, confirmed against
 * examples/libh2o/simple.c's own send-once-then-wait-for-proceed
 * pattern): the core only calls proceed() when it is ready for the
 * *next* chunk. Calling h2o_send() again before that would violate the
 * contract, so a push that arrives while a previous send has not yet
 * been "proceed()"-acknowledged is queued (single-slot, latest wins --
 * a narration stream only needs eventual consistency, not a perfect
 * backlog) instead of sent immediately.
 *
 * The 64 Hz zonetick (on_zonetick_timer_fire() in
 * webtransport_server.c, via mud_http_flush_streams() below) is
 * therefore NOT a per-tick send to every client -- that would mean 64
 * SSE frames/sec/client, real bandwidth this session's own earlier
 * "NIC traffic is the bottleneck" finding already argued against. It
 * is a periodic safety-net flush: if a client is ready and has
 * queued content waiting, send it now, matching the tick rate the
 * rest of the fabric already runs at without adding a second timer.
 * ------------------------------------------------------------------- */

#define MUD_SSE_MAX_CLIENTS 64
#define MUD_SSE_PENDING_CAP 512

typedef struct {
    char session_id[64];
    h2o_req_t *req;
    h2o_generator_t generator;
    bool in_use;
    bool ready; /* true once core has proceed()-acknowledged the last send */
    bool has_pending;
    char pending[MUD_SSE_PENDING_CAP];
    size_t pending_len;
} mud_sse_client_t;

static mud_sse_client_t g_sse_clients[MUD_SSE_MAX_CLIENTS];

static mud_sse_client_t *sse_client_from_generator(h2o_generator_t *self) {
    return (mud_sse_client_t *)((char *)self - offsetof(mud_sse_client_t, generator));
}

static void sse_send_now(mud_sse_client_t *client, const char *text, size_t len) {
    h2o_iovec_t frame = h2o_concat(&client->req->pool, h2o_iovec_init(H2O_STRLIT("data: ")),
                                    h2o_iovec_init(text, len), h2o_iovec_init(H2O_STRLIT("\n\n")));
    h2o_send(client->req, &frame, 1, H2O_SEND_STATE_IN_PROGRESS);
    client->ready = false;
}

/* Pushes `text` to every connected SSE client subscribed to
 * `session_id`. Real callers: on_mud_command() below (immediate push
 * of each turn's own narration) and, indirectly, mud_http_flush_streams()
 * for anything that could not be sent immediately. */
static void sse_push(const char *session_id, const char *text, size_t len) {
    for (int i = 0; i < MUD_SSE_MAX_CLIENTS; i++) {
        mud_sse_client_t *client = &g_sse_clients[i];
        if (!client->in_use || strcmp(client->session_id, session_id) != 0) {
            continue;
        }
        if (client->ready) {
            sse_send_now(client, text, len);
        } else {
            size_t copy_len = len < MUD_SSE_PENDING_CAP ? len : MUD_SSE_PENDING_CAP;
            memcpy(client->pending, text, copy_len);
            client->pending_len = copy_len;
            client->has_pending = true;
        }
    }
}

void mud_http_flush_streams(void) {
    for (int i = 0; i < MUD_SSE_MAX_CLIENTS; i++) {
        mud_sse_client_t *client = &g_sse_clients[i];
        if (client->in_use && client->ready && client->has_pending) {
            sse_send_now(client, client->pending, client->pending_len);
            client->has_pending = false;
        }
    }
}

static void sse_proceed(h2o_generator_t *self, h2o_req_t *req) {
    (void)req;
    mud_sse_client_t *client = sse_client_from_generator(self);
    client->ready = true;
    if (client->has_pending) {
        sse_send_now(client, client->pending, client->pending_len);
        client->has_pending = false;
    }
}

static void sse_stop(h2o_generator_t *self, h2o_req_t *req) {
    (void)req;
    mud_sse_client_t *client = sse_client_from_generator(self);
    memset(client, 0, sizeof(*client));
}

static int on_mud_stream(h2o_handler_t *self, h2o_req_t *req) {
    (void)self;
    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET"))) {
        return -1;
    }

    const char *query = req->query_at != SIZE_MAX ? req->path.base + req->query_at + 1 : NULL;
    size_t query_len = query != NULL ? req->path.len - req->query_at - 1 : 0;
    const char *session_id = query != NULL ? get_query_param(query, query_len, H2O_STRLIT("session_id")) : NULL;
    if (session_id == NULL) {
        send_json_error(req, 400, "missing session_id query parameter");
        return 0;
    }

    mud_sse_client_t *client = NULL;
    for (int i = 0; i < MUD_SSE_MAX_CLIENTS; i++) {
        if (!g_sse_clients[i].in_use) {
            client = &g_sse_clients[i];
            break;
        }
    }
    if (client == NULL) {
        send_json_error(req, 503, "too many concurrent mud streams");
        return 0;
    }

    memset(client, 0, sizeof(*client));
    snprintf(client->session_id, sizeof(client->session_id), "%s", session_id);
    client->req = req;
    client->generator.proceed = sse_proceed;
    client->generator.stop = sse_stop;
    client->in_use = true;
    client->ready = false; /* the send below is the "first chunk," matching
                             * simple.c's own send-immediately-after-start
                             * pattern -- ready flips true on the next
                             * real proceed() from core. */

    req->res.status = 200;
    req->res.reason = "OK";
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/event-stream"));
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CACHE_CONTROL, NULL, H2O_STRLIT("no-cache"));
    h2o_start_response(req, &client->generator);
    h2o_iovec_t hello = h2o_iovec_init(H2O_STRLIT(": mud stream connected\n\n"));
    h2o_send(req, &hello, 1, H2O_SEND_STATE_IN_PROGRESS);
    return 0;
}

/* ---------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------- */

static h2o_pathconf_t *register_handler(h2o_hostconf_t *hostconf, const char *path, int (*on_req)(h2o_handler_t *, h2o_req_t *)) {
    h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
    h2o_handler_t *handler = h2o_create_handler(pathconf, sizeof(*handler));
    handler->on_req = on_req;
    return pathconf;
}

void mud_http_register(h2o_hostconf_t *hostconf, const char *docroot, const char *orchestrator_path, const char *guest_elf_path) {
    mud_session_init(orchestrator_path, guest_elf_path);

    register_handler(hostconf, "/api/mud/command", on_mud_command);
    register_handler(hostconf, "/api/mud/stream", on_mud_stream);

    /* Static site last, at "/" -- h2o_file_register serves everything
     * not matched by the two more specific paths above (h2o matches
     * the longest registered path prefix first, its own documented
     * routing rule). */
    h2o_pathconf_t *root = h2o_config_register_path(hostconf, "/", 0);
    h2o_file_register(root, docroot, NULL, NULL, 0);
}

/* ---------------------------------------------------------------------
 * Real TCP listener + h2o_accept() wiring. mud_http_register() above
 * only registers paths on a hostconf/pathconf -- nothing produces an
 * h2o_req_t at all without this, matching examples/libh2o/simple.c's
 * own evloop create_listener()/on_accept() pattern (the only working
 * evloop-mode HTTP listener example this h2o checkout ships).
 * ------------------------------------------------------------------- */

static h2o_accept_ctx_t g_mud_accept_ctx;

static void on_mud_accept(h2o_socket_t *listener, const char *err) {
    if (err != NULL) {
        return;
    }
    h2o_socket_t *sock = h2o_evloop_socket_accept(listener);
    if (sock == NULL) {
        return;
    }
    h2o_accept(&g_mud_accept_ctx, sock);
}

static int setup_tls(const char *cert_file, const char *key_file) {
    g_mud_accept_ctx.ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (g_mud_accept_ctx.ssl_ctx == NULL) {
        return -1;
    }
    SSL_CTX_set_options(g_mud_accept_ctx.ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    if (SSL_CTX_use_certificate_chain_file(g_mud_accept_ctx.ssl_ctx, cert_file) != 1) {
        fprintf(stderr, "mud_http: failed to load TLS cert %s\n", cert_file);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(g_mud_accept_ctx.ssl_ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "mud_http: failed to load TLS key %s\n", key_file);
        return -1;
    }
    /* ALPN negotiates HTTP/2 when the client offers it, plain HTTP/1.1
     * otherwise -- h2o's own dispatch inside h2o_accept() handles both
     * once ssl_ctx is set, no separate code path needed here. */
    h2o_ssl_register_alpn_protocols(g_mud_accept_ctx.ssl_ctx, h2o_http2_alpn_protocols);
    return 0;
}

int mud_http_listen(h2o_context_t *ctx, h2o_loop_t *loop, int port, const char *cert_file, const char *key_file) {
    g_mud_accept_ctx.ctx = ctx;
    g_mud_accept_ctx.hosts = ctx->globalconf->hosts;
    g_mud_accept_ctx.ssl_ctx = NULL;

    if (cert_file != NULL && key_file != NULL) {
        if (setup_tls(cert_file, key_file) != 0) {
            return -1;
        }
    } else {
        fprintf(stderr, "mud_http: no TLS cert/key given, serving plain HTTP -- smoke-test mode only\n");
    }

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons((uint16_t)port);

    int reuseaddr_flag = 1;
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag, sizeof(reuseaddr_flag)) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, SOMAXCONN) != 0) {
        fprintf(stderr, "mud_http: failed to bind/listen on port %d\n", port);
        if (fd != -1) {
            close(fd);
        }
        return -1;
    }

    h2o_socket_t *sock = h2o_evloop_socket_create(loop, fd, H2O_SOCKET_FLAG_DONT_READ);
    h2o_socket_read_start(sock, on_mud_accept);

    fprintf(stderr, "mud_http: listening on %s TCP %d\n", cert_file != NULL ? "HTTPS" : "HTTP", port);
    return 0;
}
