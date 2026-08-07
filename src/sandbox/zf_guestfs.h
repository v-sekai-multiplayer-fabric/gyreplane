#ifndef ZF_GUESTFS_H_
#define ZF_GUESTFS_H_

#define FDB_API_VERSION 730

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <foundationdb/fdb_c.h>

/*
 * FoundationDB-backed guest virtual file system -- RFD 0094's item 2
 * (the capability table's storage half), with three constraints from
 * the same decision thread:
 *
 *   1. Hard limits. Every ceiling is a compile-visible constant in
 *      zf_guestfs_limits_t, enforced in this layer, not in callers.
 *      RFD 0092 makes budget *extension* a ReBAC-authorized relation
 *      later; these are the floor values a guest gets before any grant.
 *
 *   2. Offline illusion. The guest sees ordinary file I/O. Some of
 *      those "offline" calls are networked calls in disguise: this
 *      layer turns them into FDB transactions. The guest never learns
 *      which, and never touches the h2o event loop -- every FDB call
 *      here BLOCKS on its own future, which is only safe because
 *      sandbox_guest.cpp runs this on a dedicated guest thread, never
 *      on a worker loop (libfdb_c's network thread is separate and
 *      keeps running; see fdb_database.h's threading notes).
 *
 *   3. Exceeded limits become latency, never guest errors. FDB caps
 *      values at 100,000 bytes and transactions at 10 MB / 5 seconds.
 *      A real disk has no such caps -- a slow disk just blocks. So
 *      files are chunked into ~8 KB values (FDB's own recommended
 *      value size), and a flush that would exceed a transaction
 *      budget splits into several sequential transactions. The same
 *      rule covers usage: a flush that would push the zone's usage
 *      counter past max_total_bytes BLOCKS and polls until space
 *      frees, instead of returning an error. The guest experiences a
 *      hung write, exactly like a stalled drive. Consequence, stated
 *      plainly: with a single guest and nothing else freeing space,
 *      that block does not resolve until an admin-plane actor unlinks
 *      files or raises the quota -- which is RFD 0092's
 *      budget-extension-as-ReBAC-relation, not an error path here.
 *
 * Keyspace (matches zf_kv.h's prefix + big-endian style):
 *   "zf/guestfs/{z_id}/{path}\0m"            -> u64 file size (metadata)
 *   "zf/guestfs/{z_id}/{path}\0c{idx_be32}"  -> chunk content, <= 8 KB
 *   "zf/guestfs/{z_id}!u"                    -> u64 total-bytes usage
 *
 * Paths are normalized WASI-style before keying (lexical "."/".."
 * resolution inside a closed namespace) so one guest file is always
 * exactly one key family and the quota counter cannot drift.
 */

#define ZF_GUESTFS_CHUNK_BYTES             8192
#define ZF_GUESTFS_MAX_FILE_BYTES_DEFAULT  (1024 * 1024)
#define ZF_GUESTFS_MAX_TOTAL_BYTES_DEFAULT (8 * 1024 * 1024)
#define ZF_GUESTFS_MAX_OPEN_DEFAULT        32
#define ZF_GUESTFS_MAX_PATH_DEFAULT        512
/* Conservative per-transaction flush budget: far below FDB's 10 MB
 * cap, and small enough that one transaction stays well under the
 * 5-second transaction lifetime even on a congested cluster. */
#define ZF_GUESTFS_TXN_BUDGET_BYTES        (512 * 1024)

typedef struct {
    uint64_t max_file_bytes;  /* per file (policy, not an FDB limit) */
    uint64_t max_total_bytes; /* per zone guest namespace */
    uint32_t max_open_files;  /* concurrently open handles */
    uint32_t max_path_len;    /* bytes, excluding NUL */
} zf_guestfs_limits_t;

typedef struct zf_guestfs zf_guestfs_t;

/* Creates its own FDBDatabase from cluster_file -- deliberately NOT a
 * borrowed fdb_thread_state_t: the guest thread must never share the
 * event-loop adapter. Requires fdb_setup_network()/fdb_run_network()
 * to already be live (main.c does both before guests start). */
zf_guestfs_t *zf_guestfs_create(const char *cluster_file, uint32_t z_id,
                                const zf_guestfs_limits_t *limits);
void zf_guestfs_destroy(zf_guestfs_t *fs);

/* All calls below return >= 0 on success or a negative errno value
 * (-ENOENT, -EMFILE, -ENAMETOOLONG, -EIO), ready to hand to
 * machine.set_result() unchanged. They may block on any number of
 * sequential FDB transactions, and on quota pressure they poll until
 * space frees -- slow-disk semantics, never capacity errors. */

/* O_CREAT is the only honored open flag bit; everything else in the
 * guest's flags is accepted and ignored (a VFS file is always
 * read-write). Returns a handle id. */
int zf_guestfs_open(zf_guestfs_t *fs, const char *path, bool creat);
int zf_guestfs_close(zf_guestfs_t *fs, int handle);

/* Positional I/O; the handle carries its own offset (lseek adjusts). */
int64_t zf_guestfs_read(zf_guestfs_t *fs, int handle, uint8_t *buf, uint64_t len);
int64_t zf_guestfs_write(zf_guestfs_t *fs, int handle, const uint8_t *buf, uint64_t len);
int64_t zf_guestfs_lseek(zf_guestfs_t *fs, int handle, int64_t offset, int whence);

/* Size of an open handle's file, for fstat. */
int64_t zf_guestfs_size(zf_guestfs_t *fs, int handle);

/* Size by path, for newfstatat: -ENOENT when absent. */
int64_t zf_guestfs_stat_size(zf_guestfs_t *fs, const char *path);

int zf_guestfs_unlink(zf_guestfs_t *fs, const char *path);

#endif
