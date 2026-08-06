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

static void encode_u32_be(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)val;
}

size_t mud_kv_turn_key(uint8_t *buf, const char *session_id, size_t session_id_len, uint32_t turn) {
    size_t prefix_len = strlen(SS_MUD_TURN);
    memcpy(buf, SS_MUD_TURN, prefix_len);
    memcpy(buf + prefix_len, session_id, session_id_len);
    size_t off = prefix_len + session_id_len;
    buf[off++] = '/';
    encode_u32_be(buf + off, turn);
    return off + 4;
}

size_t mud_kv_turn_range_begin(uint8_t *buf, const char *session_id, size_t session_id_len) {
    size_t prefix_len = strlen(SS_MUD_TURN);
    memcpy(buf, SS_MUD_TURN, prefix_len);
    memcpy(buf + prefix_len, session_id, session_id_len);
    size_t off = prefix_len + session_id_len;
    buf[off++] = '/';
    return off;
}

size_t mud_kv_turn_range_end(uint8_t *buf, const char *session_id, size_t session_id_len) {
    /* 0xFF sentinel, exclusive upper bound -- same reasoning as
     * zf_kv_entity_range_end (ASCII-prefixed keys, 0xFF is never a
     * valid next byte). */
    size_t off = mud_kv_turn_range_begin(buf, session_id, session_id_len);
    buf[off++] = 0xFF;
    return off;
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
