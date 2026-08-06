/*
 * zone-server-h2o main entry point.
 *
 * Status: transport + basic-ZoneTick spike (plan step 0 / task #11).
 * Boots the h2o event-loop + worker-pool + FDB scaffold inherited from
 * weftspun/h2o-bench-tpcc; thread 0 additionally binds a QUIC/UDP
 * listener via src/transport/webtransport_server.c (vendored picoquic),
 * driving a bare `position += velocity * dt` ZoneTick off received QUIC
 * datagrams. See that file's header for exactly what is and is not wired
 * yet (QUIC transport: yes; negotiated WebTransport/H3 sessions: not yet).
 *
 * Only thread 0 binds the UDP port -- multiple threads calling
 * webtransport_server_init() on the same port would need SO_REUSEPORT
 * and a sharding strategy across picoquic_quic_t contexts, which is out
 * of scope for this first slice.
 *
 * Usage:
 *   zone-server-h2o -a<thread_count> -c<cluster_file> [-p<port>]
 */

#include <h2o.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "error.h"
#include "global_data.h"
#include "thread.h"
#include "transport/webtransport_server.h"

#define DEFAULT_PORT 7443 /* matches zone-server's UDP 7443, per zone.ex's x-webtransport spec */

typedef struct {
    h2o_context_t h2o_ctx;
    h2o_loop_t *loop;
    fdb_thread_state_t fdb_state;
    pthread_t tid;
    config_t *config;
    bool running;
    bool bind_transport;
    int port;
    webtransport_server_t wt_server;
} thread_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -a<thread_count> -c<cluster_file> [-p<port>]\n"
            "\n"
            "  -a  Number of worker threads\n"
            "  -c  FoundationDB cluster file path\n"
            "  -p  QUIC/UDP port for the transport, thread 0 only (default 7443)\n",
            prog);
}

static void *worker_main(void *arg)
{
    thread_ctx_t *tctx = (thread_ctx_t *)arg;

    if (tctx->bind_transport) {
        /* No cert/key wired in yet -- picoquic_create tolerates NULL for
         * an initial no-TLS-handshake smoke test of the transport bridge
         * itself; a real cert/key path (matching zone-server's
         * TLS_CERT/TLS_KEY) is needed before this can accept a real QUIC
         * client handshake. Tracked as part of finishing task #11. */
        if (webtransport_server_init(&tctx->wt_server, tctx->loop, tctx->port, NULL, NULL) != 0) {
            fprintf(stderr, "zone-server-h2o: WebTransport transport init failed on port %d\n",
                    tctx->port);
        }
    }

    while (tctx->running) {
        h2o_evloop_run(tctx->loop, INT32_MAX);
    }

    if (tctx->bind_transport) {
        webtransport_server_close(&tctx->wt_server);
    }

    return NULL;
}

static fdb_global_t fdb_global;

int main(int argc, char *argv[])
{
    config_t config = {0};
    config.fdb_cluster_file = "/etc/foundationdb/fdb.cluster";
    config.worker_count = 1;
    int port = DEFAULT_PORT;

    int opt;
    while ((opt = getopt(argc, argv, "a:c:p:h")) != -1) {
        switch (opt) {
        case 'a': config.worker_count = (size_t)atoi(optarg); break;
        case 'c': config.fdb_cluster_file = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'h':
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    if (fdb_global_init(&fdb_global, config.fdb_cluster_file, config.worker_count)) {
        fprintf(stderr, "Failed to initialize FoundationDB\n");
        return 1;
    }

    h2o_config_init(&config.h2o_config);

    thread_ctx_t *threads = calloc(config.worker_count, sizeof(*threads));

    for (size_t i = 0; i < config.worker_count; i++) {
        thread_ctx_t *t = &threads[i];
        t->config = &config;
        t->running = true;
        t->bind_transport = (i == 0);
        t->port = port;
        t->loop = h2o_evloop_create();

        h2o_context_init(&t->h2o_ctx, t->loop, &config.h2o_config);
        fdb_thread_init(&fdb_global, t->loop, &t->fdb_state);

        pthread_create(&t->tid, NULL, worker_main, t);
    }

    fprintf(stderr, "zone-server-h2o: %zu worker(s), QUIC transport on port %d (thread 0 only)\n",
            config.worker_count, port);

    fdb_run_network();

    for (size_t i = 0; i < config.worker_count; i++) {
        threads[i].running = false;
        pthread_join(threads[i].tid, NULL);
        h2o_context_dispose(&threads[i].h2o_ctx);
        fdb_thread_cleanup(&threads[i].fdb_state);
    }

    free(threads);
    cleanup_fdb_global();

    return 0;
}
