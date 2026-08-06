/*
 * MUD keyspace encoding for FoundationDB. See mud_kv.h for scope and
 * provenance -- same prefix+packed-value convention as src/zf_kv.c.
 */

#include "mud_kv.h"

#include <string.h>

size_t mud_kv_session_key(uint8_t *buf, const char *session_id, size_t session_id_len) {
    size_t prefix_len = strlen(SS_MUD_SESSION);
    memcpy(buf, SS_MUD_SESSION, prefix_len);
    memcpy(buf + prefix_len, session_id, session_id_len);
    return prefix_len + session_id_len;
}

void mud_kv_encode_session(uint8_t *buf, const mud_session_val_t *val) {
    memcpy(buf, val, sizeof(*val));
}

void mud_kv_decode_session(const uint8_t *buf, int len, mud_session_val_t *val) {
    memset(val, 0, sizeof(*val));
    if (len >= MUD_SESSION_VAL_SIZE) {
        memcpy(val, buf, sizeof(*val));
    }
}
