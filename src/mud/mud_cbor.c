#include "mud_cbor.h"

#include <stdlib.h>
#include <string.h>

#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "qcbor/qcbor_spiffy_decode.h"

static mud_cbor_buf_t finish_encode(QCBOREncodeContext *ctx, uint8_t *storage, size_t cap) {
    UsefulBufC out;
    QCBORError err = QCBOREncode_Finish(ctx, &out);
    mud_cbor_buf_t buf = {0};
    if (err != QCBOR_SUCCESS) {
        free(storage);
        return buf; /* data=NULL, len=0 -- caller checks */
    }
    /* QCBOREncode_Finish() writes into `storage` in place and returns
     * a view of it; keep the same allocation as buf.data so the
     * caller's mud_cbor_buf_free() has one real owned pointer. */
    (void)cap;
    buf.data = storage;
    buf.len = out.len;
    return buf;
}

mud_cbor_buf_t mud_cbor_encode_boot_config(int64_t seed, const char *domain, const char *objective, const char *marked_target, int64_t max_turns) {
    size_t cap = 512;
    uint8_t *storage = (uint8_t *)malloc(cap);
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){storage, cap});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapSZ(&ctx, "@context", "https://v-sekai-multiplayer-fabric.dev/mud/v1");
    QCBOREncode_AddSZStringToMapSZ(&ctx, "@type", "MudBootConfig");
    QCBOREncode_AddInt64ToMapSZ(&ctx, "seed", seed);
    /* domain: "middleham" (default) or "the_gyre" (RFD 0085 in
     * multiplayer-fabric-manuals) -- selects which room set and
     * objective mud_guest.cpp's mud_boot() starts. Omitted entirely
     * when NULL/empty so an older guest binary that predates this
     * field still boots the same way it always did. */
    if (domain != NULL && domain[0] != '\0') {
        QCBOREncode_AddSZStringToMapSZ(&ctx, "domain", domain);
    }
    QCBOREncode_AddSZStringToMapSZ(&ctx, "objective", objective);
    if (marked_target != NULL && marked_target[0] != '\0') {
        QCBOREncode_AddSZStringToMapSZ(&ctx, "marked_target", marked_target);
    }
    QCBOREncode_AddInt64ToMapSZ(&ctx, "max_turns", max_turns);
    QCBOREncode_CloseMap(&ctx);
    return finish_encode(&ctx, storage, cap);
}

mud_cbor_buf_t mud_cbor_encode_command(const char *command, const char **args, size_t n_args, const char *message) {
    size_t cap = 1024;
    uint8_t *storage = (uint8_t *)malloc(cap);
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){storage, cap});
    QCBOREncode_OpenMap(&ctx);
    QCBOREncode_AddSZStringToMapSZ(&ctx, "@type", "MudCommand");
    QCBOREncode_AddSZStringToMapSZ(&ctx, "command", command);
    QCBOREncode_OpenArrayInMapSZ(&ctx, "args");
    for (size_t k = 0; k < n_args; k++) {
        QCBOREncode_AddSZString(&ctx, args[k]);
    }
    QCBOREncode_CloseArray(&ctx);
    QCBOREncode_AddSZStringToMapSZ(&ctx, "message", message != NULL ? message : "");
    QCBOREncode_CloseMap(&ctx);
    return finish_encode(&ctx, storage, cap);
}

void mud_cbor_buf_free(mud_cbor_buf_t *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

const uint8_t *mud_cbor_map_get_str(const uint8_t *data, size_t len, const char *key, size_t *out_len) {
    QCBORDecodeContext dctx;
    QCBORDecode_Init(&dctx, (UsefulBufC){data, len}, QCBOR_DECODE_MODE_NORMAL);
    QCBORItem top;
    QCBORDecode_EnterMap(&dctx, &top);
    UsefulBufC s;
    QCBORDecode_GetTextStringInMapSZ(&dctx, key, &s);
    if (QCBORDecode_GetError(&dctx) != QCBOR_SUCCESS) {
        QCBORDecode_Finish(&dctx);
        return NULL;
    }
    *out_len = s.len;
    /* Safe to return a pointer into `data`: QCBOR's decoder never
     * copies string bytes, it only returns views into the original
     * buffer, and the caller (mud_session.c) keeps that buffer alive
     * for as long as it reads this pointer. */
    return (const uint8_t *)s.ptr;
}

bool mud_cbor_map_get_bool(const uint8_t *data, size_t len, const char *key, bool default_value) {
    QCBORDecodeContext dctx;
    QCBORDecode_Init(&dctx, (UsefulBufC){data, len}, QCBOR_DECODE_MODE_NORMAL);
    QCBORItem top;
    QCBORDecode_EnterMap(&dctx, &top);
    bool v = default_value;
    QCBORDecode_GetBoolInMapSZ(&dctx, key, &v);
    if (QCBORDecode_GetError(&dctx) != QCBOR_SUCCESS) {
        return default_value;
    }
    return v;
}

int64_t mud_cbor_map_get_int(const uint8_t *data, size_t len, const char *key, int64_t default_value) {
    QCBORDecodeContext dctx;
    QCBORDecode_Init(&dctx, (UsefulBufC){data, len}, QCBOR_DECODE_MODE_NORMAL);
    QCBORItem top;
    QCBORDecode_EnterMap(&dctx, &top);
    int64_t v = default_value;
    QCBORDecode_GetInt64InMapSZ(&dctx, key, &v);
    if (QCBORDecode_GetError(&dctx) != QCBOR_SUCCESS) {
        return default_value;
    }
    return v;
}
