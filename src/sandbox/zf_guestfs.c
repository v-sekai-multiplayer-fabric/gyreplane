/*
 * FoundationDB-backed guest VFS. See zf_guestfs.h for the contract
 * (hard limits, offline illusion, exceeded-limits-block-like-a-slow-
 * disk, guest-thread-only blocking).
 *
 * Write path: writes accumulate in the handle's in-memory buffer and
 * flush to FDB on close, chunked into ZF_GUESTFS_CHUNK_BYTES values
 * and split across as many sequential transactions as the
 * ZF_GUESTFS_TXN_BUDGET_BYTES budget requires. The guest blocks for
 * the duration -- slow-disk semantics. A flush that would exceed the
 * zone's usage quota blocks and polls (100 ms) until space frees.
 *
 * Every FDB call blocks via fdb_future_block_until_ready. That is safe
 * here and only here: sandbox_guest.cpp calls this from the dedicated
 * guest pthread, never from an h2o worker loop. libfdb_c's own network
 * thread (fdb_run_network in main) services the futures meanwhile.
 */

#include "zf_guestfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GFS_PREFIX "zf/guestfs/"
#define GFS_KEY_MAX 700
#define GFS_QUOTA_POLL_NS (100 * 1000 * 1000) /* 100 ms */

typedef struct {
    bool     in_use;
    bool     dirty;
    char    *path;      /* malloc'd, already normalized */
    uint8_t *buf;       /* file content, max_file_bytes capacity */
    uint64_t size;
    uint64_t pos;
    uint64_t committed_size; /* size last seen in FDB, for quota delta */
} gfs_handle_t;

struct zf_guestfs {
    FDBDatabase        *db;
    uint32_t            z_id;
    zf_guestfs_limits_t limits;
    gfs_handle_t       *handles; /* limits.max_open_files entries */
};

/*
 * Path normalization, WASI-style: the guest namespace is closed (every
 * path becomes a key under this zone's prefix, never a host path), so
 * escape is structurally impossible -- but without normalization,
 * "a/../b" and "b" would be two different key families for what the
 * guest believes is one file, and the quota counter would drift.
 * WASI's filesystem capability model resolves exactly this class
 * before path lookup (resolve "." and ".." lexically inside the
 * namespace, refuse ".." past the root), so this follows that model.
 *
 * Output: components joined by '/', no leading '/', no "." or "..".
 * Returns length, or -1 if ".." would climb above the root or the
 * result exceeds max_len.
 */
static int gfs_normalize_path(const char *in, char *out, size_t max_len)
{
    size_t out_len = 0;
    size_t comp_starts[64];
    size_t n_comps = 0;

    const char *p = in;
    while (*p == '/') p++; /* guest absolute and relative paths unify */

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        while (*p == '/') p++;

        if (clen == 0 || (clen == 1 && start[0] == '.')) continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            if (n_comps == 0) return -1; /* would climb above the root */
            n_comps--;
            out_len = comp_starts[n_comps];
            continue;
        }
        if (n_comps >= 64) return -1;
        if (out_len + clen + 2 > max_len) return -1;
        comp_starts[n_comps++] = out_len;
        if (out_len > 0) out[out_len++] = '/';
        memcpy(out + out_len, start, clen);
        out_len += clen;
    }
    out[out_len] = '\0';
    return (int)out_len;
}

/* --- keys ------------------------------------------------------------- */

static size_t gfs_key_base(const zf_guestfs_t *fs, uint8_t *buf, const char *path)
{
    size_t n = 0;
    memcpy(buf, GFS_PREFIX, sizeof(GFS_PREFIX) - 1);
    n += sizeof(GFS_PREFIX) - 1;
    buf[n++] = (uint8_t)(fs->z_id >> 24);
    buf[n++] = (uint8_t)(fs->z_id >> 16);
    buf[n++] = (uint8_t)(fs->z_id >> 8);
    buf[n++] = (uint8_t)(fs->z_id);
    buf[n++] = '/';
    size_t plen = strlen(path);
    memcpy(buf + n, path, plen);
    n += plen;
    buf[n++] = '\0'; /* terminator keeps "a" and "ab" key families apart */
    return n;
}

static size_t gfs_meta_key(const zf_guestfs_t *fs, uint8_t *buf, const char *path)
{
    size_t n = gfs_key_base(fs, buf, path);
    buf[n++] = 'm';
    return n;
}

static size_t gfs_chunk_key(const zf_guestfs_t *fs, uint8_t *buf, const char *path,
                            uint32_t idx)
{
    size_t n = gfs_key_base(fs, buf, path);
    buf[n++] = 'c';
    buf[n++] = (uint8_t)(idx >> 24);
    buf[n++] = (uint8_t)(idx >> 16);
    buf[n++] = (uint8_t)(idx >> 8);
    buf[n++] = (uint8_t)(idx);
    return n;
}

static size_t gfs_usage_key(const zf_guestfs_t *fs, uint8_t *buf)
{
    size_t n = 0;
    memcpy(buf, GFS_PREFIX, sizeof(GFS_PREFIX) - 1);
    n += sizeof(GFS_PREFIX) - 1;
    buf[n++] = (uint8_t)(fs->z_id >> 24);
    buf[n++] = (uint8_t)(fs->z_id >> 16);
    buf[n++] = (uint8_t)(fs->z_id >> 8);
    buf[n++] = (uint8_t)(fs->z_id);
    buf[n++] = '!';
    buf[n++] = 'u';
    return n;
}

/* --- blocking FDB helpers --------------------------------------------- */

/* Sentinel: txn function asks the outer loop to sleep and retry the
 * whole transaction (quota pressure -- slow-disk blocking). */
#define GFS_EAGAIN_QUOTA (-1000000)

/* Runs fn(tr, ctx) inside the standard retry loop. fn returns 0 to
 * commit, GFS_EAGAIN_QUOTA to block-and-retry, or a negative errno to
 * abort (returned as-is). */
typedef int (*gfs_txn_fn)(zf_guestfs_t *fs, FDBTransaction *tr, void *ctx);

static int gfs_txn(zf_guestfs_t *fs, gfs_txn_fn fn, void *ctx)
{
    FDBTransaction *tr = NULL;
    fdb_error_t err = fdb_database_create_transaction(fs->db, &tr);
    if (err) return -EIO;

    for (;;) {
        int rc = fn(fs, tr, ctx);
        if (rc == GFS_EAGAIN_QUOTA) {
            /* Quota pressure blocks like a stalled drive: sleep, then
             * retry on a fresh transaction so the usage re-reads. */
            struct timespec ts = { 0, GFS_QUOTA_POLL_NS };
            nanosleep(&ts, NULL);
            fdb_transaction_reset(tr);
            continue;
        }
        if (rc < 0) {
            fdb_transaction_destroy(tr);
            return rc;
        }

        FDBFuture *cf = fdb_transaction_commit(tr);
        fdb_future_block_until_ready(cf);
        err = fdb_future_get_error(cf);
        fdb_future_destroy(cf);
        if (!err) {
            fdb_transaction_destroy(tr);
            return 0;
        }

        FDBFuture *rf = fdb_transaction_on_error(tr, err);
        fdb_future_block_until_ready(rf);
        fdb_error_t rerr = fdb_future_get_error(rf);
        fdb_future_destroy(rf);
        if (rerr) { /* not retryable */
            fdb_transaction_destroy(tr);
            return -EIO;
        }
        /* retryable: tr is reset by on_error; loop again */
    }
}

/* Blocking point-get inside tr. On present, fills buf (up to buf_len)
 * and *out_len with the full value length. Returns 1 present,
 * 0 absent, -EIO. */
static int gfs_get(FDBTransaction *tr, const uint8_t *key, int key_len,
                   uint8_t *buf, int buf_len, int *out_len)
{
    FDBFuture *f = fdb_transaction_get(tr, key, key_len, 0);
    fdb_future_block_until_ready(f);
    if (fdb_future_get_error(f)) { fdb_future_destroy(f); return -EIO; }

    fdb_bool_t present = 0;
    const uint8_t *val = NULL;
    int len = 0;
    if (fdb_future_get_value(f, &present, &val, &len)) {
        fdb_future_destroy(f);
        return -EIO;
    }
    if (present && buf) {
        int n = len < buf_len ? len : buf_len;
        memcpy(buf, val, (size_t)n);
    }
    if (out_len) *out_len = len;
    fdb_future_destroy(f);
    return present ? 1 : 0;
}

static uint64_t gfs_decode_u64(const uint8_t *v, int len)
{
    uint64_t x = 0;
    if (len == 8) memcpy(&x, v, 8);
    return x;
}

/* --- lifecycle -------------------------------------------------------- */

zf_guestfs_t *zf_guestfs_create(const char *cluster_file, uint32_t z_id,
                                const zf_guestfs_limits_t *limits)
{
    zf_guestfs_t *fs = calloc(1, sizeof(*fs));
    if (!fs) return NULL;
    fs->z_id = z_id;
    fs->limits = *limits;
    if (fs->limits.max_file_bytes == 0) {
        fs->limits.max_file_bytes = ZF_GUESTFS_MAX_FILE_BYTES_DEFAULT;
    }
    fs->handles = calloc(fs->limits.max_open_files, sizeof(gfs_handle_t));
    if (!fs->handles) { free(fs); return NULL; }

    if (fdb_create_database(cluster_file, &fs->db)) {
        free(fs->handles);
        free(fs);
        return NULL;
    }
    return fs;
}

void zf_guestfs_destroy(zf_guestfs_t *fs)
{
    if (!fs) return;
    for (uint32_t i = 0; i < fs->limits.max_open_files; i++) {
        if (fs->handles[i].in_use) zf_guestfs_close(fs, (int)i);
    }
    fdb_database_destroy(fs->db);
    free(fs->handles);
    free(fs);
}

/* --- open (load): meta + chunks, possibly several transactions -------- */

typedef struct {
    gfs_handle_t *h;
    bool          creat;
    bool          found;
} gfs_load_meta_ctx_t;

static int gfs_load_meta_txn(zf_guestfs_t *fs, FDBTransaction *tr, void *vctx)
{
    gfs_load_meta_ctx_t *ctx = vctx;
    uint8_t key[GFS_KEY_MAX];
    size_t key_len = gfs_meta_key(fs, key, ctx->h->path);

    uint8_t sz[8]; int len = 0;
    int rc = gfs_get(tr, key, (int)key_len, sz, 8, &len);
    if (rc < 0) return rc;
    ctx->found = rc == 1;
    if (ctx->found) {
        uint64_t size = gfs_decode_u64(sz, len);
        if (size > fs->limits.max_file_bytes) size = fs->limits.max_file_bytes;
        ctx->h->size = size;
    }
    return 0;
}

typedef struct {
    gfs_handle_t *h;
    uint32_t      first_chunk;
    uint32_t      n_chunks;
} gfs_load_chunks_ctx_t;

static int gfs_load_chunks_txn(zf_guestfs_t *fs, FDBTransaction *tr, void *vctx)
{
    gfs_load_chunks_ctx_t *ctx = vctx;
    for (uint32_t i = 0; i < ctx->n_chunks; i++) {
        uint32_t idx = ctx->first_chunk + i;
        uint64_t off = (uint64_t)idx * ZF_GUESTFS_CHUNK_BYTES;
        if (off >= ctx->h->size) break;
        uint64_t want = ctx->h->size - off;
        if (want > ZF_GUESTFS_CHUNK_BYTES) want = ZF_GUESTFS_CHUNK_BYTES;

        uint8_t key[GFS_KEY_MAX];
        size_t key_len = gfs_chunk_key(fs, key, ctx->h->path, idx);
        int len = 0;
        int rc = gfs_get(tr, key, (int)key_len, ctx->h->buf + off, (int)want, &len);
        if (rc < 0) return rc;
        /* Absent chunk inside the recorded size reads as zeros --
         * sparse-file semantics, same as a real disk. */
    }
    return 0;
}

int zf_guestfs_open(zf_guestfs_t *fs, const char *path, bool creat)
{
    char norm[ZF_GUESTFS_MAX_PATH_DEFAULT + 1];
    if (gfs_normalize_path(path, norm, sizeof(norm)) < 0) return -ENAMETOOLONG;
    if (strlen(norm) > fs->limits.max_path_len) return -ENAMETOOLONG;

    int idx = -1;
    for (uint32_t i = 0; i < fs->limits.max_open_files; i++) {
        if (!fs->handles[i].in_use) { idx = (int)i; break; }
    }
    if (idx < 0) return -EMFILE;

    gfs_handle_t *h = &fs->handles[idx];
    h->path = strdup(norm);
    h->buf = calloc(1, fs->limits.max_file_bytes);
    if (!h->path || !h->buf) {
        free(h->path); free(h->buf);
        memset(h, 0, sizeof(*h));
        return -EIO;
    }
    h->size = 0;
    h->pos = 0;
    h->dirty = false;
    h->committed_size = 0;

    gfs_load_meta_ctx_t mctx = { .h = h, .creat = creat, .found = false };
    int rc = gfs_txn(fs, gfs_load_meta_txn, &mctx);
    if (rc == 0 && !mctx.found && !creat) rc = -ENOENT;
    if (rc == 0 && mctx.found && h->size > 0) {
        /* Chunk loads split across transactions under the same budget
         * as flushes: a big open is a slow open, never an error. */
        uint32_t per_txn = ZF_GUESTFS_TXN_BUDGET_BYTES / ZF_GUESTFS_CHUNK_BYTES;
        uint32_t total = (uint32_t)((h->size + ZF_GUESTFS_CHUNK_BYTES - 1) /
                                    ZF_GUESTFS_CHUNK_BYTES);
        for (uint32_t first = 0; rc == 0 && first < total; first += per_txn) {
            uint32_t n = total - first < per_txn ? total - first : per_txn;
            gfs_load_chunks_ctx_t cctx = { .h = h, .first_chunk = first, .n_chunks = n };
            rc = gfs_txn(fs, gfs_load_chunks_txn, &cctx);
        }
    }
    if (rc < 0) {
        free(h->path); free(h->buf);
        memset(h, 0, sizeof(*h));
        return rc;
    }
    h->committed_size = mctx.found ? h->size : 0;
    h->in_use = true;
    h->dirty = creat && !mctx.found; /* ensure creation persists */
    return idx;
}

/* --- flush (close): quota gate, then chunks, then meta ---------------- */

typedef struct { gfs_handle_t *h; } gfs_quota_ctx_t;

static int gfs_quota_txn(zf_guestfs_t *fs, FDBTransaction *tr, void *vctx)
{
    gfs_quota_ctx_t *ctx = vctx;
    gfs_handle_t *h = ctx->h;

    uint8_t ukey[GFS_KEY_MAX];
    size_t ukey_len = gfs_usage_key(fs, ukey);
    uint8_t uv[8]; int ulen = 0;
    int rc = gfs_get(tr, ukey, (int)ukey_len, uv, 8, &ulen);
    if (rc < 0) return rc;
    uint64_t usage = rc == 1 ? gfs_decode_u64(uv, ulen) : 0;

    uint64_t new_usage = usage - h->committed_size + h->size;
    if (new_usage > fs->limits.max_total_bytes) {
        /* Over quota: the outer loop sleeps and retries -- the guest
         * blocks like a stalled drive (see zf_guestfs.h's contract). */
        return GFS_EAGAIN_QUOTA;
    }
    fdb_transaction_set(tr, ukey, (int)ukey_len, (const uint8_t *)&new_usage, 8);

    /* Meta commits with the quota, before chunk bodies: a crash
     * mid-flush leaves a file whose tail reads as zeros -- torn-write
     * semantics a real disk also has. */
    uint8_t mkey[GFS_KEY_MAX];
    size_t mkey_len = gfs_meta_key(fs, mkey, h->path);
    fdb_transaction_set(tr, mkey, (int)mkey_len, (const uint8_t *)&h->size, 8);
    return 0;
}

typedef struct {
    gfs_handle_t *h;
    uint32_t      first_chunk;
    uint32_t      n_chunks;
} gfs_flush_chunks_ctx_t;

static int gfs_flush_chunks_txn(zf_guestfs_t *fs, FDBTransaction *tr, void *vctx)
{
    gfs_flush_chunks_ctx_t *ctx = vctx;
    gfs_handle_t *h = ctx->h;
    for (uint32_t i = 0; i < ctx->n_chunks; i++) {
        uint32_t idx = ctx->first_chunk + i;
        uint64_t off = (uint64_t)idx * ZF_GUESTFS_CHUNK_BYTES;
        if (off >= h->size) break;
        uint64_t n = h->size - off;
        if (n > ZF_GUESTFS_CHUNK_BYTES) n = ZF_GUESTFS_CHUNK_BYTES;

        uint8_t key[GFS_KEY_MAX];
        size_t key_len = gfs_chunk_key(fs, key, h->path, idx);
        fdb_transaction_set(tr, key, (int)key_len, h->buf + off, (int)n);
    }
    return 0;
}

/* Chunks past the new size must clear (shrink case). */
typedef struct { gfs_handle_t *h; } gfs_trim_ctx_t;

static int gfs_trim_txn(zf_guestfs_t *fs, FDBTransaction *tr, void *vctx)
{
    gfs_trim_ctx_t *ctx = vctx;
    gfs_handle_t *h = ctx->h;
    uint32_t keep = (uint32_t)((h->size + ZF_GUESTFS_CHUNK_BYTES - 1) /
                               ZF_GUESTFS_CHUNK_BYTES);
    uint8_t begin[GFS_KEY_MAX], end[GFS_KEY_MAX];
    size_t blen = gfs_chunk_key(fs, begin, h->path, keep);
    size_t elen = gfs_chunk_key(fs, end, h->path, UINT32_MAX);
    fdb_transaction_clear_range(tr, begin, (int)blen, end, (int)elen);
    return 0;
}

int zf_guestfs_close(zf_guestfs_t *fs, int handle)
{
    if (handle < 0 || (uint32_t)handle >= fs->limits.max_open_files) return -EBADF;
    gfs_handle_t *h = &fs->handles[handle];
    if (!h->in_use) return -EBADF;

    int rc = 0;
    if (h->dirty) {
        gfs_quota_ctx_t qctx = { .h = h };
        rc = gfs_txn(fs, gfs_quota_txn, &qctx);

        uint32_t per_txn = ZF_GUESTFS_TXN_BUDGET_BYTES / ZF_GUESTFS_CHUNK_BYTES;
        uint32_t total = (uint32_t)((h->size + ZF_GUESTFS_CHUNK_BYTES - 1) /
                                    ZF_GUESTFS_CHUNK_BYTES);
        for (uint32_t first = 0; rc == 0 && first < total; first += per_txn) {
            uint32_t n = total - first < per_txn ? total - first : per_txn;
            gfs_flush_chunks_ctx_t cctx = { .h = h, .first_chunk = first, .n_chunks = n };
            rc = gfs_txn(fs, gfs_flush_chunks_txn, &cctx);
        }
        if (rc == 0 && h->size < h->committed_size) {
            gfs_trim_ctx_t tctx = { .h = h };
            rc = gfs_txn(fs, gfs_trim_txn, &tctx);
        }
    }
    free(h->path);
    free(h->buf);
    memset(h, 0, sizeof(*h));
    return rc;
}

/* --- I/O (in-memory; the buffer is the "page cache") ------------------ */

static gfs_handle_t *gfs_h(zf_guestfs_t *fs, int handle)
{
    if (handle < 0 || (uint32_t)handle >= fs->limits.max_open_files) return NULL;
    gfs_handle_t *h = &fs->handles[handle];
    return h->in_use ? h : NULL;
}

int64_t zf_guestfs_read(zf_guestfs_t *fs, int handle, uint8_t *buf, uint64_t len)
{
    gfs_handle_t *h = gfs_h(fs, handle);
    if (!h) return -EBADF;
    if (h->pos >= h->size) return 0;
    uint64_t n = h->size - h->pos;
    if (n > len) n = len;
    memcpy(buf, h->buf + h->pos, n);
    h->pos += n;
    return (int64_t)n;
}

int64_t zf_guestfs_write(zf_guestfs_t *fs, int handle, const uint8_t *buf, uint64_t len)
{
    gfs_handle_t *h = gfs_h(fs, handle);
    if (!h) return -EBADF;
    /* max_file_bytes is a hard policy cap (the "disk geometry"), not
     * an FDB artifact; past it, writes truncate to what fits, the way
     * a device with a fixed extent would. Zero fit blocks nothing --
     * report what was written. */
    if (h->pos >= fs->limits.max_file_bytes) return 0;
    uint64_t fit = fs->limits.max_file_bytes - h->pos;
    if (len > fit) len = fit;
    memcpy(h->buf + h->pos, buf, len);
    h->pos += len;
    if (h->pos > h->size) h->size = h->pos;
    h->dirty = true;
    return (int64_t)len;
}

int64_t zf_guestfs_lseek(zf_guestfs_t *fs, int handle, int64_t offset, int whence)
{
    gfs_handle_t *h = gfs_h(fs, handle);
    if (!h) return -EBADF;
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;                 /* SEEK_SET */
    case 1: base = (int64_t)h->pos; break;   /* SEEK_CUR */
    case 2: base = (int64_t)h->size; break;  /* SEEK_END */
    default: return -EINVAL;
    }
    int64_t npos = base + offset;
    if (npos < 0 || (uint64_t)npos > fs->limits.max_file_bytes) return -EINVAL;
    h->pos = (uint64_t)npos;
    return npos;
}

int64_t zf_guestfs_size(zf_guestfs_t *fs, int handle)
{
    gfs_handle_t *h = gfs_h(fs, handle);
    if (!h) return -EBADF;
    return (int64_t)h->size;
}

/* --- path ops --------------------------------------------------------- */

typedef struct { const char *path; int64_t size; } gfs_stat_ctx_t;

static int gfs_stat_txn(zf_guestfs_t *fs, FDBTransaction *tr, void *vctx)
{
    gfs_stat_ctx_t *ctx = vctx;
    uint8_t key[GFS_KEY_MAX];
    size_t key_len = gfs_meta_key(fs, key, ctx->path);
    uint8_t sz[8]; int len = 0;
    int rc = gfs_get(tr, key, (int)key_len, sz, 8, &len);
    if (rc < 0) return rc;
    if (rc == 0) return -ENOENT;
    ctx->size = (int64_t)gfs_decode_u64(sz, len);
    return 0;
}

int64_t zf_guestfs_stat_size(zf_guestfs_t *fs, const char *path)
{
    char norm[ZF_GUESTFS_MAX_PATH_DEFAULT + 1];
    if (gfs_normalize_path(path, norm, sizeof(norm)) < 0) return -ENAMETOOLONG;
    gfs_stat_ctx_t ctx = { .path = norm, .size = 0 };
    int rc = gfs_txn(fs, gfs_stat_txn, &ctx);
    return rc < 0 ? rc : ctx.size;
}

typedef struct { const char *path; } gfs_unlink_ctx_t;

static int gfs_unlink_txn(zf_guestfs_t *fs, FDBTransaction *tr, void *vctx)
{
    gfs_unlink_ctx_t *ctx = vctx;

    uint8_t mkey[GFS_KEY_MAX];
    size_t mkey_len = gfs_meta_key(fs, mkey, ctx->path);
    uint8_t sz[8]; int len = 0;
    int rc = gfs_get(tr, mkey, (int)mkey_len, sz, 8, &len);
    if (rc < 0) return rc;
    if (rc == 0) return -ENOENT;
    uint64_t fsize = gfs_decode_u64(sz, len);

    uint8_t ukey[GFS_KEY_MAX];
    size_t ukey_len = gfs_usage_key(fs, ukey);
    uint8_t uv[8]; int ulen = 0;
    rc = gfs_get(tr, ukey, (int)ukey_len, uv, 8, &ulen);
    if (rc < 0) return rc;
    uint64_t usage = rc == 1 ? gfs_decode_u64(uv, ulen) : 0;
    uint64_t new_usage = usage > fsize ? usage - fsize : 0;

    /* Clear the whole key family: base terminator '\0' keeps this
     * range from touching any sibling path's keys. */
    uint8_t begin[GFS_KEY_MAX], end[GFS_KEY_MAX];
    size_t blen = gfs_key_base(fs, begin, ctx->path);
    size_t elen = blen;
    memcpy(end, begin, blen);
    end[elen - 1] = 1; /* '\0' -> 0x01: covers every suffix */
    fdb_transaction_clear_range(tr, begin, (int)blen, end, (int)elen);
    fdb_transaction_set(tr, ukey, (int)ukey_len, (const uint8_t *)&new_usage, 8);
    return 0;
}

int zf_guestfs_unlink(zf_guestfs_t *fs, const char *path)
{
    char norm[ZF_GUESTFS_MAX_PATH_DEFAULT + 1];
    if (gfs_normalize_path(path, norm, sizeof(norm)) < 0) return -ENAMETOOLONG;
    gfs_unlink_ctx_t ctx = { .path = norm };
    return gfs_txn(fs, gfs_unlink_txn, &ctx);
}
