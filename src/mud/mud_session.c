#include "mud_session.h"
#include "mud_cbor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MUD_SESSION_MAX 256
#define MUD_SESSION_ID_MAX 64

struct mud_session {
    char session_id[MUD_SESSION_ID_MAX];
    pid_t pid;
    int to_child_fd;   /* write commands here (child's stdin) */
    int from_child_fd; /* read results here (child's stdout) */
    bool in_use;
};

static struct mud_session g_sessions[MUD_SESSION_MAX];
static char g_orchestrator_path[512];
static char g_guest_elf_path[512];

void mud_session_init(const char *orchestrator_path, const char *guest_elf_path) {
    memset(g_sessions, 0, sizeof(g_sessions));
    snprintf(g_orchestrator_path, sizeof(g_orchestrator_path), "%s", orchestrator_path);
    snprintf(g_guest_elf_path, sizeof(g_guest_elf_path), "%s", guest_elf_path);
}

/* FNV-1a, real seed derivation from a client-minted session id -- the
 * MUD's own seed just needs to be stable per session, not adversary-
 * resistant (no accounts, no stakes beyond the game itself, matching
 * "no need for oauth"). */
static uint64_t session_id_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool write_all(int fd, const uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static bool read_all(int fd, uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, data + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false; /* EOF or real error -- child likely crashed */
        }
        off += (size_t)n;
    }
    return true;
}

/* Length-prefixed framing, matching mud/orchestrator/main.cpp's own
 * read_frame()/write_frame(): 4-byte little-endian length, then that
 * many CBOR bytes. */
static bool send_frame(int fd, const uint8_t *data, size_t len) {
    uint8_t hdr[4] = {
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF), (uint8_t)((len >> 24) & 0xFF),
    };
    return write_all(fd, hdr, 4) && write_all(fd, data, len);
}

static int recv_frame(int fd, uint8_t **out, size_t *out_len) {
    uint8_t hdr[4];
    if (!read_all(fd, hdr, 4)) {
        return -1;
    }
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (len > 0 && !read_all(fd, buf, len)) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

static mud_session_t *find_session(const char *session_id) {
    for (int i = 0; i < MUD_SESSION_MAX; i++) {
        if (g_sessions[i].in_use && strcmp(g_sessions[i].session_id, session_id) == 0) {
            return &g_sessions[i];
        }
    }
    return NULL;
}

static mud_session_t *alloc_session_slot(void) {
    for (int i = 0; i < MUD_SESSION_MAX; i++) {
        if (!g_sessions[i].in_use) {
            return &g_sessions[i];
        }
    }
    return NULL;
}

/* Spawns mud-sandbox-orchestrator with its stdin/stdout redirected to
 * a pair of real pipes, following exactly the posix_spawn_file_actions
 * pattern -- posix_spawn(), not fork()+exec(), avoids the real
 * multi-threaded-fork hazards h2o's own worker threads would create
 * (this project already runs h2o on multiple pthreads, main.c's own
 * worker_main()). */
static bool spawn_orchestrator(mud_session_t *s) {
    int to_child[2];  /* [0]=child reads (its stdin), [1]=we write */
    int from_child[2]; /* [0]=we read, [1]=child writes (its stdout) */
    if (pipe(to_child) != 0) {
        return false;
    }
    if (pipe(from_child) != 0) {
        close(to_child[0]);
        close(to_child[1]);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, to_child[1]);
    posix_spawn_file_actions_addclose(&actions, from_child[0]);

    char *argv[] = {g_orchestrator_path, g_guest_elf_path, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, g_orchestrator_path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(to_child[0]);
    close(from_child[1]);
    if (rc != 0) {
        close(to_child[1]);
        close(from_child[0]);
        return false;
    }

    s->pid = pid;
    s->to_child_fd = to_child[1];
    s->from_child_fd = from_child[0];
    return true;
}

mud_session_t *mud_session_get_or_create(const char *session_id, const char *domain, const char *objective) {
    mud_session_t *existing = find_session(session_id);
    if (existing != NULL) {
        return existing;
    }

    mud_session_t *s = alloc_session_slot();
    if (s == NULL) return NULL; /* MUD_SESSION_MAX concurrent sessions reached */

    snprintf(s->session_id, sizeof(s->session_id), "%s", session_id);
    if (!spawn_orchestrator(s)) {
        memset(s, 0, sizeof(*s));
        return NULL;
    }

    int64_t seed = (int64_t)(session_id_hash(session_id) & 0x7FFFFFFF);
    mud_cbor_buf_t cfg = mud_cbor_encode_boot_config(seed, domain, objective, NULL, 50);
    bool sent = send_frame(s->to_child_fd, cfg.data, cfg.len);
    mud_cbor_buf_free(&cfg);
    if (!sent) {
        close(s->to_child_fd);
        close(s->from_child_fd);
        kill(s->pid, SIGKILL);
        memset(s, 0, sizeof(*s));
        return NULL;
    }

    uint8_t *ack;
    size_t ack_len;
    if (recv_frame(s->from_child_fd, &ack, &ack_len) != 0) {
        close(s->to_child_fd);
        close(s->from_child_fd);
        kill(s->pid, SIGKILL);
        memset(s, 0, sizeof(*s));
        return NULL;
    }
    bool booted = mud_cbor_map_get_bool(ack, ack_len, "ok", false);
    free(ack);
    if (!booted) {
        close(s->to_child_fd);
        close(s->from_child_fd);
        kill(s->pid, SIGKILL);
        memset(s, 0, sizeof(*s));
        return NULL;
    }

    s->in_use = true;
    return s;
}

int mud_session_step(mud_session_t *session, const uint8_t *cmd_cbor, size_t cmd_len, uint8_t **out, size_t *out_len) {
    if (!send_frame(session->to_child_fd, cmd_cbor, cmd_len)) {
        return -1;
    }
    return recv_frame(session->from_child_fd, out, out_len);
}

void mud_session_close_all(void) {
    for (int i = 0; i < MUD_SESSION_MAX; i++) {
        if (!g_sessions[i].in_use) {
            continue;
        }
        close(g_sessions[i].to_child_fd);
        close(g_sessions[i].from_child_fd);
        kill(g_sessions[i].pid, SIGKILL);
        int status;
        waitpid(g_sessions[i].pid, &status, 0);
        memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
    }
}
