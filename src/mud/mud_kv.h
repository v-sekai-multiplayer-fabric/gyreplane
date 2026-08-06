#ifndef MUD_KV_H_
#define MUD_KV_H_

#define FDB_API_VERSION 730

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <foundationdb/fdb_c.h>

/*
 * MUD keyspace for FoundationDB. Same convention as src/zf_kv.h (this
 * project's own zonefabric keyspace) -- prefix + key bytes, packed
 * binary values -- reused directly rather than inventing a second
 * style, per the plan's own step 3 note.
 *
 * FDB here is the durable, crash-recoverable record of a MUD session's
 * high-level state (per the decision doc: the libriscv guest's own
 * in-memory Machine<W> state is the live/fast copy; FDB is the source
 * of truth across restarts). It does not store the world's full room/
 * NPC graph -- that is deterministically rebuilt from (seed,
 * objective) every time mud_boot() runs, matching
 * MiddlehamStateMachine.__init__'s own deterministic-from-seed
 * construction. Only what cannot be deterministically rederived --
 * how many turns have run, whether the session is finished -- needs a
 * durable record at all.
 *
 * Unlike zf_kv.h's zone/entity ids (dense u32, big-endian-encoded so
 * range scans sort correctly), a MUD session id is a client-minted,
 * arbitrary-length string (no accounts, per "no need for oauth") --
 * stored as raw key bytes after the prefix, still real byte-
 * lexicographic FDB key ordering, just not fixed-width.
 */

#define SS_MUD_SESSION "zf/mud/session/"
#define SS_MUD_TURN "zf/mud/turn/"

/* "zf/mud/session/{session_id}". Writes into buf (caller-sized to at
 * least strlen(SS_MUD_SESSION) + session_id_len), returns length. */
size_t mud_kv_session_key(uint8_t *buf, const char *session_id, size_t session_id_len);

/* "zf/mud/turn/{session_id}/{turn_be32}" -- one key per turn, value is
 * the raw narration text (no packed struct; plain UTF-8 bytes are a
 * real, valid FDB value on their own). Big-endian turn number keeps
 * the range in chronological order under one session's own prefix,
 * matching zf_kv.h's own big-endian-key convention for the same
 * reason (lexicographic key order == real order). */
size_t mud_kv_turn_key(uint8_t *buf, const char *session_id, size_t session_id_len, uint32_t turn);
size_t mud_kv_turn_range_begin(uint8_t *buf, const char *session_id, size_t session_id_len);
size_t mud_kv_turn_range_end(uint8_t *buf, const char *session_id, size_t session_id_len);

#pragma pack(push, 1)
typedef struct {
    uint32_t seed;
    int32_t turn;
    uint8_t finished;   /* 0/1 */
    uint8_t objective_complete; /* 0/1 */
    char objective[32]; /* "gain_watch_trust" / "identify_marked_contact", NUL-padded */
} mud_session_val_t;
#pragma pack(pop)

#define MUD_SESSION_VAL_SIZE ((int)sizeof(mud_session_val_t))

void mud_kv_encode_session(uint8_t *buf, const mud_session_val_t *val);
void mud_kv_decode_session(const uint8_t *buf, int len, mud_session_val_t *val);

#endif // MUD_KV_H_
