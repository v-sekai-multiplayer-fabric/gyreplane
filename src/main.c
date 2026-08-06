/*
 * zone-server-h2o main entry point.
 *
 * Status: transport + basic-ZoneTick spike (plan step 0 / task #11).
 * No WebTransport/QUIC listener yet -- this boots the h2o event-loop +
 * worker-pool + FDB scaffold inherited from weftspun/h2o-bench-tpcc and
 * stands up an empty per-thread FDB-connected worker, ready for the
 * WebTransport datagram handler and a bare `position += velocity * dt`
 * ZoneTick to be wired in next.
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

#define DEFAULT_PORT 7443 /* matches zone-server's UDP 7443, per zone.ex's x-webtransport spec */

typedef struct {
    h2o_context_t h2o_ctx;
    h2o_loop_t *loop;
    fdb_thread_state_t fdb_state;
    pthread_t tid;
    config_t *config;
    bool running;
} thread_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -a<thread_count> -c<cluster_file> [-p<port>]\n"
            "\n"
            "  -a  Number of worker threads\n"
            "  -c  FoundationDB cluster file path\n"
            "  -p  Port placeholder (default 7443; no listener bound yet)\n",
            prog);
}

static void *worker_main(void *arg)
{
    thread_ctx_t *tctx = (thread_ctx_t *)arg;

    /* TODO(task #11): bind the WebTransport/QUIC datagram listener here
     * (pending the libh2o WebTransport spike), and drive a bare ZoneTick
     * (position += velocity * dt, no physics) off it per plan step 0. */
    while (tctx->running) {
        h2o_evloop_run(tctx->loop, INT32_MAX);
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
        t->loop = h2o_evloop_create();

        h2o_context_init(&t->h2o_ctx, t->loop, &config.h2o_config);
        fdb_thread_init(&fdb_global, t->loop, &t->fdb_state);

        pthread_create(&t->tid, NULL, worker_main, t);
    }

    fprintf(stderr, "zone-server-h2o: %zu worker(s), port %d (listener not yet bound)\n",
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
