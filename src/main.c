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
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "error.h"
#include "global_data.h"
#include "thread.h"
#include "transport/webtransport_server.h"

#define DEFAULT_PORT 7443 /* matches zone-server's UDP 7443, per zone.ex's x-webtransport spec */

/* zone-server (Godot)'s scripts/start.sh writes its TLS_CERT/TLS_KEY Fly
 * secrets (raw PEM content) to disk before exec-ing the binary, since
 * picoquic (like Godot's own TLS stack) takes file paths, not PEM
 * buffers, for picoquic_create. zone-server-h2o is a drop-in for that
 * same Fly secrets pair: if TLS_CERT/TLS_KEY are set, write them here
 * the same way, into the same shape start.sh uses. -t/-k below are the
 * CLI alternative, for local/dev use with
 * scripts/generate-tls-cert.sh's own output files directly, no env
 * vars needed. */
#define CERT_TMP_DIR "/tmp/zone_server_h2o_certs"

/* Returns 0 and fills cert_file_out/key_file_out on success (already
 * pointing at real files -- either the caller's -t/-k paths verbatim,
 * or freshly written copies of TLS_CERT/TLS_KEY's env content), or -1
 * on a real error. Leaves both NULL (not an error) if neither the CLI
 * flags nor the env vars are set -- picoquic_create's own NULL/NULL
 * no-TLS-handshake smoke-test mode, unchanged from before this was
 * wired in. */
static int resolve_tls_files(const char *cli_cert, const char *cli_key,
                              const char **cert_file_out, const char **key_file_out)
{
    if (cli_cert && cli_key) {
        *cert_file_out = cli_cert;
        *key_file_out = cli_key;
        return 0;
    }

    const char *env_cert = getenv("TLS_CERT");
    const char *env_key = getenv("TLS_KEY");
    if (!env_cert || !env_key) {
        *cert_file_out = NULL;
        *key_file_out = NULL;
        return 0;
    }

    if (mkdir(CERT_TMP_DIR, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "zone-server-h2o: mkdir %s failed: %s\n", CERT_TMP_DIR, strerror(errno));
        return -1;
    }

    static char cert_path[sizeof(CERT_TMP_DIR) + 16];
    static char key_path[sizeof(CERT_TMP_DIR) + 16];
    snprintf(cert_path, sizeof(cert_path), "%s/cert.pem", CERT_TMP_DIR);
    snprintf(key_path, sizeof(key_path), "%s/key.pem", CERT_TMP_DIR);

    FILE *f = fopen(cert_path, "w");
    if (!f) {
        fprintf(stderr, "zone-server-h2o: could not write %s: %s\n", cert_path, strerror(errno));
        return -1;
    }
    fputs(env_cert, f);
    fclose(f);

    int key_fd = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (key_fd < 0) {
        fprintf(stderr, "zone-server-h2o: could not write %s: %s\n", key_path, strerror(errno));
        return -1;
    }
    FILE *kf = fdopen(key_fd, "w");
    fputs(env_key, kf);
    fclose(kf);

    *cert_file_out = cert_path;
    *key_file_out = key_path;
    return 0;
}

typedef struct {
    h2o_context_t h2o_ctx;
    h2o_loop_t *loop;
    fdb_thread_state_t fdb_state;
    pthread_t tid;
    config_t *config;
    bool running;
    bool bind_transport;
    int port;
    uint32_t z_id;
    const char *cert_file;
    const char *key_file;
    webtransport_server_t wt_server;
} thread_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -a<thread_count> -c<cluster_file> -z<zone_id> [-p<port>] [-t<cert_file> -k<key_file>]\n"
            "\n"
            "  -a  Number of worker threads\n"
            "  -c  FoundationDB cluster file path\n"
            "  -z  This process's zone ID -- a zone fabric is multiple\n"
            "      processes, each one zone (1 process : 1 zone), matching\n"
            "      zone-server/AGENTS.md's one-UDP-port-per-zone-instance\n"
            "      deployment shape. Required.\n"
            "  -p  QUIC/UDP port for the transport, thread 0 only (default 7443)\n"
            "  -t  TLS cert PEM file (see scripts/generate-tls-cert.sh).\n"
            "      Needs -k too. Falls back to the TLS_CERT/TLS_KEY env\n"
            "      vars (raw PEM content, zone-server's own Fly secrets\n"
            "      convention) if neither is given. No TLS handshake\n"
            "      (smoke-test mode) if none of the above are set.\n"
            "  -k  TLS private key PEM file. See -t.\n",
            prog);
}

static void *worker_main(void *arg)
{
    thread_ctx_t *tctx = (thread_ctx_t *)arg;

    if (tctx->bind_transport) {
        if (webtransport_server_init(&tctx->wt_server, tctx->loop, tctx->port,
                                      tctx->cert_file, tctx->key_file,
                                      &tctx->fdb_state, tctx->z_id) != 0) {
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
    long z_id = -1;
    bool have_z_id = false;
    const char *cli_cert = NULL;
    const char *cli_key = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "a:c:p:z:t:k:h")) != -1) {
        switch (opt) {
        case 'a': config.worker_count = (size_t)atoi(optarg); break;
        case 'c': config.fdb_cluster_file = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'z': z_id = atol(optarg); have_z_id = true; break;
        case 't': cli_cert = optarg; break;
        case 'k': cli_key = optarg; break;
        case 'h':
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    if (!have_z_id || z_id < 0) {
        fprintf(stderr, "zone-server-h2o: -z<zone_id> is required "
                        "(a zone fabric is multiple processes, each one zone)\n");
        usage(argv[0]);
        return 1;
    }

    if ((cli_cert != NULL) != (cli_key != NULL)) {
        fprintf(stderr, "zone-server-h2o: -t and -k must be given together\n");
        usage(argv[0]);
        return 1;
    }

    const char *cert_file;
    const char *key_file;
    if (resolve_tls_files(cli_cert, cli_key, &cert_file, &key_file) != 0) {
        return 1;
    }
    if (!cert_file) {
        fprintf(stderr, "zone-server-h2o: no TLS cert/key given (-t/-k or "
                        "TLS_CERT/TLS_KEY) -- running with no TLS handshake, "
                        "smoke-test mode only\n");
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
        t->z_id = (uint32_t)z_id;
        t->cert_file = cert_file;
        t->key_file = key_file;
        t->loop = h2o_evloop_create();

        h2o_context_init(&t->h2o_ctx, t->loop, &config.h2o_config);
        fdb_thread_init(&fdb_global, t->loop, &t->fdb_state);

        pthread_create(&t->tid, NULL, worker_main, t);
    }

    fprintf(stderr, "zone-server-h2o: zone %u, %zu worker(s), QUIC transport on port %d (thread 0 only)\n",
            (uint32_t)z_id, config.worker_count, port);

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
