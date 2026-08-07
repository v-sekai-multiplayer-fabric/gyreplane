#ifndef MUD_CBOR_H_
#define MUD_CBOR_H_

/*
 * mud_cbor: the host (zone-server-h2o) side's small encode/decode
 * surface for the mud-sandbox-orchestrator boundary's CBOR + JSON-LD
 * framed messages (mud_boot config, mud_step command/result).
 *
 * Wraps thirdparty/QCBOR (vendored per multiplayer-fabric-manuals
 * RFD 0001's own decision: QCBOR over zcbor, plain-CBOR RFC 8949
 * decoding inside the host process, no hand-written codec) -- this
 * file only narrows QCBOR's own general API down to the handful of
 * calls mud_session.c/mud_http.c actually need, it does not
 * reimplement CBOR encoding/decoding itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- encoding ---- */
typedef struct {
    uint8_t *data; /* owned, caller frees via mud_cbor_buf_free() */
    size_t len;
} mud_cbor_buf_t;

/* Builds the mud_boot() config: {"@context", "@type": "MudBootConfig",
 * "seed", "objective", "marked_target", "max_turns"}, matching the
 * decision doc's own MudBootConfig shape. */
mud_cbor_buf_t mud_cbor_encode_boot_config(int64_t seed, const char *domain, const char *objective, const char *marked_target, int64_t max_turns);

/* Builds one mud_step() command: {"@type": "MudCommand", "command",
 * "args": [...], "message"}, matching the decision doc's own
 * MudCommand shape. */
mud_cbor_buf_t mud_cbor_encode_command(const char *command, const char **args, size_t n_args, const char *message);

void mud_cbor_buf_free(mud_cbor_buf_t *buf);

/* ---- decoding a MudTurnResult (or MudBootAck) top-level field ---- */

/* Returns a pointer into `data` (NOT nul-terminated -- use *out_len)
 * on success, NULL if the key is absent or not a string. */
const uint8_t *mud_cbor_map_get_str(const uint8_t *data, size_t len, const char *key, size_t *out_len);
bool mud_cbor_map_get_bool(const uint8_t *data, size_t len, const char *key, bool default_value);
int64_t mud_cbor_map_get_int(const uint8_t *data, size_t len, const char *key, int64_t default_value);

#endif // MUD_CBOR_H_
