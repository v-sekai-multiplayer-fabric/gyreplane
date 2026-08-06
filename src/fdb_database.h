#ifndef FDB_DATABASE_H_
#define FDB_DATABASE_H_

#define FDB_API_VERSION 730

#include <h2o.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <foundationdb/fdb_c.h>

#include "list.h"

/*
 * Async FDB database adapter for h2o-bench-tpcc.
 *
 * Replaces the libpq connection pool (database.c) with FDB's C API.
 * FDB futures are integrated into h2o's event loop via
 * fdb_future_set_callback + h2o_timer_t for timeout handling.
 *
 * Each worker thread gets its own FDBDatabase* handle (thread-safe
 * after fdb_setup_network). Transactions are created per-request.
 *
 * FDB API version: 730 (FDB 7.3.x)
 */

#define FDB_CLUSTER_FILE_DEFAULT "/etc/foundationdb/fdb.cluster"
#define FDB_DB_ERROR "FDB error"
#define FDB_DB_TIMEOUT_ERROR "FDB transaction timeout"

typedef enum { FDB_DONE = 1 } fdb_result_return_t;

/* A single FDB operation submitted to the adapter. */
typedef struct fdb_op_param {
    /* The transaction this operation belongs to. */
    FDBTransaction *tr;

    /* Callbacks */
    void (*on_result)(struct fdb_op_param *param, FDBFuture *future);
    void (*on_error)(struct fdb_op_param *param, fdb_error_t err);
    void (*on_timeout)(struct fdb_op_param *param);

    /* User data */
    void *ctx;

    /* For callback chaining */
    list_t l;
} fdb_op_param_t;

/* Per-thread FDB state. */
typedef struct {
    FDBDatabase *db;
    h2o_loop_t *loop;
    h2o_timer_t timer;
    size_t active_transactions;
    size_t max_transactions;
} fdb_thread_state_t;

/* Global FDB state. */
typedef struct {
    const char *cluster_file;
    fdb_thread_state_t *thread_states;
    size_t num_threads;
    bool network_started;
} fdb_global_t;

/* Initialize the global FDB network (call once at startup). */
int fdb_global_init(fdb_global_t *fdb, const char *cluster_file, size_t num_threads);

/* Initialize per-thread FDB state. */
int fdb_thread_init(fdb_global_t *fdb, h2o_loop_t *loop, fdb_thread_state_t *state);

/* Create a transaction on the given thread's FDB database. */
int fdb_create_transaction(fdb_thread_state_t *state, FDBTransaction **tr);

/* Execute a get operation asynchronously. */
int fdb_async_get(fdb_thread_state_t *state, FDBTransaction *tr,
                  const uint8_t *key, int key_len,
                  void (*cb)(FDBFuture *, void *), void *ctx);

/* Execute a set operation (synchronous within the transaction). */
void fdb_sync_set(FDBTransaction *tr,
                  const uint8_t *key, int key_len,
                  const uint8_t *value, int value_len);

/* Execute a range get operation asynchronously. */
int fdb_async_get_range(fdb_thread_state_t *state, FDBTransaction *tr,
                        const uint8_t *begin, int begin_len,
                        const uint8_t *end, int end_len,
                        void (*cb)(FDBFuture *, void *), void *ctx);

/* Commit a transaction asynchronously. */
int fdb_async_commit(fdb_thread_state_t *state, FDBTransaction *tr,
                     void (*cb)(FDBFuture *, void *), void *ctx);

/* Handle a transaction error (retry or fail). */
int fdb_handle_error(fdb_thread_state_t *state, FDBTransaction *tr,
                     fdb_error_t err,
                     void (*cb)(FDBFuture *, void *), void *ctx);

/* Cleanup. */
void fdb_thread_cleanup(fdb_thread_state_t *state);
void fdb_global_cleanup(fdb_global_t *fdb);

#endif
