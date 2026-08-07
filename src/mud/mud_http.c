#include "mud_http.h"
#include "mud_cbor.h"
#include "mud_kv.h"
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
 * Real FDB durability for MUD turns. Set once via mud_http_set_fdb_state()
 * (main.c's own worker_main(), thread 0 only -- see mud_http.h). NULL
 * (never set) means every write/read below is a real, silent no-op,
 * matching every other optional feature in this file.
 * ------------------------------------------------------------------- */
static fdb_thread_state_t *g_fdb_state = NULL;

void mud_http_set_fdb_state(fdb_thread_state_t *state) { g_fdb_state = state; }

typedef struct {
    char session_id[64];
    uint32_t turn;
} mud_turn_write_ctx_t;

static void on_mud_turn_write_commit(FDBFuture *future, void *arg) {
    mud_turn_write_ctx_t *ctx = (mud_turn_write_ctx_t *)arg;
    fdb_error_t err = fdb_future_get_error(future);
    if (err) {
        fprintf(stderr, "mud_http: FDB write failed for session %s turn %u: %s\n",
                ctx->session_id, ctx->turn, fdb_get_error(err));
    }
    /* No fdb_future_destroy() here on purpose: fdb_database.c's own
     * future_callback() destroys the future exactly once, right after
     * this returns. Destroying it here too was a real double-free that
     * segfaulted inside libfdb_c on every single MUD command. See
     * fdb_database.h's contract note on fdb_async_commit(). */
    free(ctx);
}

/* Fire-and-forget: one blind write per turn, a fresh key every time
 * (turn number is part of the key), so there is no real read-modify-
 * write conflict to retry on -- unlike zf_zonetick.c's own
 * range-read-then-write pattern, which does need
 * fdb_handle_error()'s retry loop. */
static void mud_kv_write_turn_async(const char *session_id, uint32_t turn, const char *narration, size_t narration_len) {
    if (g_fdb_state == NULL) return;
    FDBTransaction *tr;
    if (fdb_create_transaction(g_fdb_state, &tr) != 0) return;
    uint8_t key[128];
    size_t key_len = mud_kv_turn_key(key, session_id, strlen(session_id), turn);
    fdb_sync_set(tr, key, (int)key_len, (const uint8_t *)narration, (int)narration_len);
    mud_turn_write_ctx_t *ctx = (mud_turn_write_ctx_t *)malloc(sizeof(*ctx));
    snprintf(ctx->session_id, sizeof(ctx->session_id), "%s", session_id);
    ctx->turn = turn;
    if (fdb_async_commit(g_fdb_state, tr, on_mud_turn_write_commit, ctx) != 0) {
        fdb_transaction_destroy(tr);
        free(ctx);
    }
}

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
    /* "middleham" (default) or "the_gyre" -- only consulted on the
     * first request for a given session_id, per mud_session.c's own
     * get-or-create semantics; an existing session ignores it, the
     * same way it already ignores a resent objective. */
    char domain[32];
    /* Parser state: which top-level field the next string/array
     * belongs to. */
    enum { FIELD_NONE, FIELD_SESSION_ID, FIELD_COMMAND, FIELD_ARGS, FIELD_MESSAGE, FIELD_DOMAIN } current_field;
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
    else if (len == 6 && memcmp(key, "domain", 6) == 0) {
        req->current_field = FIELD_DOMAIN;
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
    case FIELD_DOMAIN:
        snprintf(req->domain, sizeof(req->domain), "%.*s", (int)len, val);
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

    /* domain picks the objective too, so the website's mode selector
     * (mud/web/index.html, the 3D view -- the only MUD web UI now)
     * only has to send one field, not both. Existing sessions ignore
     * both, per mud_session.c's own get-or-create semantics. */
    const char *domain = parsed.domain[0] != '\0' ? parsed.domain : "middleham";
    const char *objective = (strcmp(domain, "the_gyre") == 0) ? "explore_gyre" : "gain_watch_trust";
    mud_session_t *session = mud_session_get_or_create(parsed.session_id, domain, objective);
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
        int64_t turn = mud_cbor_map_get_int(result, result_len, "turn", 0);
        mud_kv_write_turn_async(parsed.session_id, (uint32_t)turn, (const char *)narration, narration_len);
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
    size_t session_id_len = 0;
    const char *session_id = query != NULL ? get_query_param(query, query_len, H2O_STRLIT("session_id"), &session_id_len) : NULL;
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
    /* session_id is not NUL-terminated (a raw slice of h2o's own
     * request buffer) -- "%.*s" bounds the copy at session_id_len
     * instead of trusting a NUL that is not guaranteed to be there. */
    snprintf(client->session_id, sizeof(client->session_id), "%.*s", (int)session_id_len, session_id);
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
 * GET /api/mud/history?session_id=... -- a real async FDB range read,
 * following zf_zonetick.c's own zt_start_range_read/zt_on_range_read
 * pattern (fdb_create_transaction -> fdb_async_get_range -> parse via
 * fdb_future_get_keyvalue_array), not a synchronous read forced onto
 * h2o's single-threaded evloop. The handler returns 0 without calling
 * h2o_start_response() at all -- a real, legal deferred-response h2o
 * handler, matching how on_mud_stream() above never blocks either.
 * The response itself is sent once the FDB future resolves.
 * ------------------------------------------------------------------- */

typedef struct {
    h2o_req_t *req;
    FDBTransaction *tr;
    char session_id[64];
} mud_history_ctx_t;

/* ---------------------------------------------------------------------
 * FDB's own C API contract: a future's callback runs either on the
 * thread that called fdb_future_set_callback(), or on FDB's own
 * network thread (fdb_run_network(), its own dedicated OS thread here,
 * per src/main.c's fdb_run_network() call) -- not guaranteed to be the
 * h2o worker thread that owns `req`. h2o_req_t, its pool, and
 * h2o_start_response()/h2o_send() are not safe to touch from any
 * thread but the one h2o's event loop runs on for that request.
 *
 * RFD 0073 (async-fdb-callback-chain) already documents this callback
 * chain as running inside a worker thread with results handed back to
 * h2o via h2o_multithread_send_message() (RFD 0072,
 * actor-lite-worker-pool, mirrored in src/worker_pool.c) -- this
 * handler previously skipped that hand-off and called h2o functions
 * straight from the FDB callback. Confirmed as a real bug, not a
 * theoretical one: AddressSanitizer caught a genuine SEGV inside
 * libfdb_c.so on this exact path.
 *
 * g_mud_history_receiver is registered once, on the h2o worker
 * thread's own loop, in mud_http_listen() below. on_mud_history_range_
 * read() (which may run on FDB's thread) only builds a message and
 * hands it off; on_mud_history_return() (which always runs on h2o's
 * own thread, since h2o's multithread queue delivers it there) is the
 * only place that touches `req`.
 * ------------------------------------------------------------------- */

typedef struct {
    h2o_multithread_message_t super;
    h2o_req_t *req;
    int status;
    char *body;    /* malloc'd, not req->pool -- pool is not thread-safe */
    size_t body_len;
} mud_history_return_msg_t;

static h2o_multithread_receiver_t g_mud_history_receiver;

static void on_mud_history_return(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages) {
    (void)receiver;
    while (!h2o_linklist_is_empty(messages)) {
        h2o_linklist_t *node = messages->next;
        h2o_linklist_unlink(node);
        mud_history_return_msg_t *msg = (mud_history_return_msg_t *)node;
        h2o_req_t *req = msg->req;

        static h2o_generator_t generator = {NULL, NULL};
        req->res.status = msg->status;
        req->res.reason = msg->status == 200 ? "OK" : "error";
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));
        h2o_start_response(req, &generator);
        /* h2o_strdup copies into req->pool -- safe here, this runs on
         * h2o's own thread. msg->body (malloc'd) is freed right after,
         * once its bytes are copied into the pool-owned iovec. */
        h2o_iovec_t json = h2o_strdup(&req->pool, msg->body, msg->body_len);
        h2o_send(req, &json, 1, H2O_SEND_STATE_FINAL);

        free(msg->body);
        free(msg);
    }
}

/* Called once, from mud_http_listen() on the h2o worker thread that
 * owns `ctx`, registering on ctx->queue -- h2o.h's own comment on
 * h2o_context_t::queue calls it "queue for receiving messages from
 * other contexts". h2o_multithread_create_queue() is a real, valid
 * public API (a process can have more than one queue), so making an
 * independent second queue here was not itself the bug -- reusing
 * ctx->queue is just the more idiomatic choice, matching what h2o's
 * own field comment says it is for. Confirmed NOT the cause of the
 * "Assertion !h2o_linklist_is_linked(&message->link) failed" crash
 * production hit: switching to ctx->queue alone did not stop the
 * crash under the same repeated-traffic test that reproduced it. The
 * real cause was mud_history_send_error()'s own bug -- see its
 * comment. */
static void mud_history_return_init(h2o_context_t *ctx) {
    h2o_multithread_register_receiver(ctx->queue, &g_mud_history_receiver, on_mud_history_return);
}

static void mud_history_send_error(h2o_req_t *req, int status, const char *message) {
    /* calloc, not malloc: h2o_multithread_message_t embeds an
     * h2o_linklist_t (super.link), and h2o_linklist_is_linked() (the
     * assertion h2o_multithread_send_message() runs before linking)
     * checks link.next != NULL -- malloc's uninitialized bytes are
     * not reliably NULL, so this asserted and crashed the process on
     * real production traffic ("Assertion !h2o_linklist_is_linked
     * (&message->link) failed"), not a theoretical concern. h2o.h's
     * own linklist.h says heads need h2o_linklist_init_anchor(); a
     * plain zero-fill satisfies the same next==NULL check more simply
     * for a node that isn't a list head. */
    mud_history_return_msg_t *msg = (mud_history_return_msg_t *)calloc(1, sizeof(*msg));
    msg->req = req;
    msg->status = status;
    /* strdup, not the message's own storage -- fdb_get_error() returns
     * a static string (safe to send as-is), but treating both error
     * sources identically here is simpler and just as correct. */
    msg->body_len = strlen(message);
    msg->body = (char *)malloc(msg->body_len + 16);
    int n = snprintf(msg->body, msg->body_len + 16, "{\"error\":\"%s\"}", message);
    msg->body_len = (size_t)n;
    h2o_multithread_send_message(&g_mud_history_receiver, &msg->super);
}

static void on_mud_history_range_read(FDBFuture *future, void *arg) {
    mud_history_ctx_t *ctx = (mud_history_ctx_t *)arg;
    h2o_req_t *req = ctx->req;

    /* Every path below destroys the transaction and ctx but never the
     * future -- future_callback() owns that. See fdb_database.h. */
    fdb_error_t err = fdb_future_get_error(future);
    if (err) {
        fdb_transaction_destroy(ctx->tr);
        mud_history_send_error(req, 502, fdb_get_error(err));
        free(ctx);
        return;
    }

    FDBKeyValue const *kvs;
    int count;
    fdb_bool_t more;
    err = fdb_future_get_keyvalue_array(future, &kvs, &count, &more);
    if (err) {
        fdb_transaction_destroy(ctx->tr);
        mud_history_send_error(req, 502, "failed to parse history range");
        free(ctx);
        return;
    }

    /* Build a real JSON array of narration strings, oldest first (the
     * range scan already returns them in big-endian-turn key order,
     * per mud_kv_turn_key()'s own layout). yajl's own buffer, not
     * req->pool -- this may still be running on FDB's thread. */
    yajl_gen gen = yajl_gen_alloc(NULL);
    yajl_gen_array_open(gen);
    for (int i = 0; i < count; i++) {
        yajl_gen_string(gen, kvs[i].value, (size_t)kvs[i].value_length);
    }
    yajl_gen_array_close(gen);
    const unsigned char *buf;
    size_t buf_len;
    yajl_gen_get_buf(gen, &buf, &buf_len);

    mud_history_return_msg_t *msg = (mud_history_return_msg_t *)calloc(1, sizeof(*msg)); /* see mud_history_send_error()'s own comment on why calloc, not malloc */
    msg->req = req;
    msg->status = 200;
    msg->body = (char *)malloc(buf_len);
    memcpy(msg->body, buf, buf_len);
    msg->body_len = buf_len;
    yajl_gen_free(gen);

    /* kvs points into the future's own buffer, but everything needed is
     * already copied into msg->body above, so it is safe to stop using
     * it here. The future itself is still future_callback()'s to
     * destroy, not ours. */
    fdb_transaction_destroy(ctx->tr);
    free(ctx);

    h2o_multithread_send_message(&g_mud_history_receiver, &msg->super);
}

static int on_mud_history(h2o_handler_t *self, h2o_req_t *req) {
    (void)self;
    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET"))) {
        return -1;
    }
    if (g_fdb_state == NULL) {
        send_json_error(req, 503, "FDB not configured for this process");
        return 0;
    }

    const char *query = req->query_at != SIZE_MAX ? req->path.base + req->query_at + 1 : NULL;
    size_t query_len = query != NULL ? req->path.len - req->query_at - 1 : 0;
    size_t session_id_len = 0;
    const char *session_id = query != NULL ? get_query_param(query, query_len, H2O_STRLIT("session_id"), &session_id_len) : NULL;
    if (session_id == NULL) {
        send_json_error(req, 400, "missing session_id query parameter");
        return 0;
    }
    /* session_id is not NUL-terminated -- strlen() here previously read
     * past it into whatever followed in h2o's request buffer, feeding
     * an unbounded length into mud_kv_turn_range_begin/end's memcpy
     * into these fixed 128-byte stack buffers below. A real, previously
     * shipped stack buffer overflow, not theoretical -- confirmed via
     * AddressSanitizer against this exact endpoint. session_id_len from
     * get_query_param is the query string's own real, bounded length. */
    if (session_id_len >= sizeof(((mud_history_ctx_t *)0)->session_id)) {
        send_json_error(req, 400, "session_id too long");
        return 0;
    }

    FDBTransaction *tr;
    if (fdb_create_transaction(g_fdb_state, &tr) != 0) {
        send_json_error(req, 502, "could not create FDB transaction");
        return 0;
    }

    uint8_t begin[128], end[128];
    size_t begin_len = mud_kv_turn_range_begin(begin, session_id, session_id_len);
    size_t end_len = mud_kv_turn_range_end(end, session_id, session_id_len);

    mud_history_ctx_t *ctx = (mud_history_ctx_t *)malloc(sizeof(*ctx));
    ctx->req = req;
    ctx->tr = tr;
    snprintf(ctx->session_id, sizeof(ctx->session_id), "%.*s", (int)session_id_len, session_id);

    if (fdb_async_get_range(g_fdb_state, tr, begin, (int)begin_len, end, (int)end_len,
                             on_mud_history_range_read, ctx) != 0) {
        fdb_transaction_destroy(tr);
        free(ctx);
        send_json_error(req, 502, "could not start FDB range read");
        return 0;
    }
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
    register_handler(hostconf, "/api/mud/history", on_mud_history);

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

    /* Registers g_mud_history_receiver on ctx's own queue -- this is
     * the h2o worker thread that will own every h2o_req_t the MUD
     * HTTP paths see, matching mud_http_register()'s one-listener-
     * on-zone-0 convention. See on_mud_history_range_read()'s own
     * comment for why this hand-off exists at all, and
     * mud_history_return_init()'s own comment for why it registers on
     * ctx->queue rather than a second, independent queue. */
    mud_history_return_init(ctx);

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
