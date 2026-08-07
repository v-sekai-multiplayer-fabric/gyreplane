/*
 * FoundationDB-backed guest key-value store. See zf_guest_kv.h for the
 * contract: small linearizable state only, no chunking, quota
 * pressure blocks instead of failing, guest-thread-only blocking.
 * Content goes to the object store, never here.
 *
 * Every FDB call blocks via fdb_future_block_until_ready. That is safe
 * here and only here: sandbox_guest.cpp calls this from the dedicated
 * guest pthread, never from an h2o worker loop. libfdb_c's own network
 * thread (fdb_run_network in main) services the futures meanwhile.
 */

#include "zf_guest_kv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GKV_PREFIX "zf/guestkv/"
#define GKV_KEY_MAX 700
#define GKV_QUOTA_POLL_NS (100 * 1000 * 1000) /* 100 ms */

struct zf_guest_kv {
    FDBDatabase         *db;
    uint32_t             z_id;
    zf_guest_kv_limits_t limits;
};

/*
 * Key normalization, WASI-style. The guest namespace is closed (every
 * key lands under this zone's prefix, never a host path), so escape is
 * structurally impossible -- but without normalization, "a/../b" and
 * "b" would be two different key families for what the guest believes
 * is one entry, and the usage counter would drift. WASI's filesystem
 * capability model resolves exactly this class before lookup, so this
 * follows that model.
 *
 * Output: components joined by '/', no leading '/', no "." or "..".
 * Returns length, or -1 if ".." would climb above the root or the
 * result exceeds max_len.
 */
static int gkv_normalize(const char *in, char *out, size_t max_len)
{
    size_t out_len = 0;
    size_t comp_starts[64];
    size_t n_comps = 0;

    const char *p = in;
    while (*p == '/') p++;

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

