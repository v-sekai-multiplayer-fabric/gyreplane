#ifndef MUD_QCBOR_VALUE_HPP_
#define MUD_QCBOR_VALUE_HPP_

/*
 * qcbor_value: a small C++ Value-tree convenience wrapper around
 * thirdparty/QCBOR's real encode/decode API (QCBOREncode_ family,
 * QCBORDecode_GetNext()), for the guest side of the
 * mud-sandbox-orchestrator boundary. Every byte on the wire is
 * produced/consumed by QCBOR itself -- this header only builds and
 * walks an in-memory tree shape around it, per multiplayer-fabric-
 * manuals RFD 0001's decision (QCBOR over a hand-written codec).
 *
 * mud_guest.cpp's own MiddlehamStateMachine::step() already builds and
 * reads its CBOR messages through a small Value tree (map/array/str/
 * int/bool); this header keeps that same call shape so that code did
 * not need a full rewrite, while replacing what actually walks the
 * wire bytes with QCBOR.
 */

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "qcbor/qcbor_spiffy_decode.h"

namespace mudcbor {

enum class Type { Null, Bool, Int, Str, Array, Map };

struct Value {
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> map;

    static Value null() { return Value{}; }
    static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value integer(int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value str(const std::string &v) { Value x; x.type = Type::Str; x.s = v; return x; }
    static Value array() { Value x; x.type = Type::Array; return x; }
    static Value object() { Value x; x.type = Type::Map; return x; }

    void set(const std::string &key, Value v) {
        for (auto &kv : map) {
            if (kv.first == key) { kv.second = std::move(v); return; }
        }
        map.emplace_back(key, std::move(v));
    }

    const Value *get(const std::string &key) const {
        for (auto &kv : map) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }

    std::string get_str(const std::string &key, const std::string &def = "") const {
        const Value *v = get(key);
        return (v && v->type == Type::Str) ? v->s : def;
    }

    int64_t get_int(const std::string &key, int64_t def = 0) const {
        const Value *v = get(key);
        return (v && v->type == Type::Int) ? v->i : def;
    }

    bool get_bool(const std::string &key, bool def = false) const {
        const Value *v = get(key);
        return (v && v->type == Type::Bool) ? v->b : def;
    }
};

/* ---- encode: walk the Value tree, emit it via real QCBOREncode_* ---- */

inline void encode_value(QCBOREncodeContext *ctx, const Value &v) {
    switch (v.type) {
    case Type::Null:
        QCBOREncode_AddNULL(ctx);
        break;
    case Type::Bool:
        QCBOREncode_AddBool(ctx, v.b);
        break;
    case Type::Int:
        QCBOREncode_AddInt64(ctx, v.i);
        break;
    case Type::Str:
        QCBOREncode_AddText(ctx, (UsefulBufC){v.s.data(), v.s.size()});
        break;
    case Type::Array:
        QCBOREncode_OpenArray(ctx);
        for (auto &e : v.arr) {
            encode_value(ctx, e);
        }
        QCBOREncode_CloseArray(ctx);
        break;
    case Type::Map:
        QCBOREncode_OpenMap(ctx);
        for (auto &kv : v.map) {
            QCBOREncode_AddSZString(ctx, kv.first.c_str());
            encode_value(ctx, kv.second);
        }
        QCBOREncode_CloseMap(ctx);
        break;
    }
}

inline std::vector<uint8_t> encode(const Value &v) {
    /* Two-pass: QCBOREncode_FinishGetSize() to size the buffer, then
     * encode for real -- QCBOR's own documented pattern for an
     * unknown-size output (qcbor_encode.h's own top-of-file comment). */
    QCBOREncodeContext sizing;
    QCBOREncode_Init(&sizing, (UsefulBuf){NULL, SIZE_MAX});
    encode_value(&sizing, v);
    size_t needed = 0;
    QCBOREncode_FinishGetSize(&sizing, &needed);

    std::vector<uint8_t> out(needed);
    QCBOREncodeContext ctx;
    QCBOREncode_Init(&ctx, (UsefulBuf){out.data(), out.size()});
    encode_value(&ctx, v);
    UsefulBufC result;
    QCBORError err = QCBOREncode_Finish(&ctx, &result);
    if (err != QCBOR_SUCCESS) {
        throw std::runtime_error("qcbor encode failed");
    }
    out.resize(result.len);
    return out;
}

/* ---- decode: rebuild a Value tree from QCBORDecode_GetNext()'s own
 * pre-order item stream, using uNestingLevel/uNextNestLevel to detect
 * container boundaries, matching QCBOR's own documented traversal
 * model (qcbor_decode.h's "Basic Decoding" section). ---- */

inline Value item_to_scalar(const QCBORItem &item) {
    switch (item.uDataType) {
    case QCBOR_TYPE_INT64:
        return Value::integer(item.val.int64);
    case QCBOR_TYPE_UINT64:
        return Value::integer((int64_t)item.val.uint64);
    case QCBOR_TYPE_TEXT_STRING:
        return Value::str(std::string((const char *)item.val.string.ptr, item.val.string.len));
    case QCBOR_TYPE_TRUE:
        return Value::boolean(true);
    case QCBOR_TYPE_FALSE:
        return Value::boolean(false);
    case QCBOR_TYPE_NULL:
    default:
        return Value::null();
    }
}

/* QCBOR's own traversal model (qcbor_decode.h's "Basic Decoding"
 * section): each QCBORItem carries its own uNextNestLevel, the
 * nesting level of whatever GetNext() will return *next* -- not the
 * current item's own level. That is what tells a caller whether a
 * container has more children still to come, with no need to fetch
 * one extra item just to check it (an earlier version of this
 * function did exactly that "peek one ahead" and silently dropped
 * the peeked item, corrupting every map field that followed a nested
 * array/map -- found via mud_guest.cpp's own smoke test showing
 * empty pre_room fields and truncated narration once a
 * dialogue_signal/npc_reactions sub-object preceded them). */
inline Value decode_recursive(QCBORDecodeContext *dctx, QCBORItem &item) {
    if (item.uDataType == QCBOR_TYPE_MAP) {
        Value v = Value::object();
        uint8_t container_level = item.uNestingLevel;
        while (item.uNextNestLevel > container_level) {
            QCBORItem key_item;
            if (QCBORDecode_GetNext(dctx, &key_item) != QCBOR_SUCCESS) {
                break;
            }
            std::string key(key_item.label.string.ptr ? (const char *)key_item.label.string.ptr : "",
                             key_item.label.string.len);
            v.set(key, decode_recursive(dctx, key_item));
            item = key_item; /* check the just-consumed item's own uNextNestLevel next */
        }
        return v;
    }
    if (item.uDataType == QCBOR_TYPE_ARRAY) {
        Value v = Value::array();
        uint8_t container_level = item.uNestingLevel;
        while (item.uNextNestLevel > container_level) {
            QCBORItem elem;
            if (QCBORDecode_GetNext(dctx, &elem) != QCBOR_SUCCESS) {
                break;
            }
            v.arr.push_back(decode_recursive(dctx, elem));
            item = elem;
        }
        return v;
    }
    return item_to_scalar(item);
}

inline Value decode(const uint8_t *data, size_t len) {
    QCBORDecodeContext dctx;
    QCBORDecode_Init(&dctx, (UsefulBufC){data, len}, QCBOR_DECODE_MODE_NORMAL);
    QCBORItem top;
    if (QCBORDecode_GetNext(&dctx, &top) != QCBOR_SUCCESS) {
        throw std::runtime_error("qcbor decode: empty/malformed input");
    }
    /* Note: decode_recursive()'s map/array loop above already consumes
     * every nested item via repeated QCBORDecode_GetNext() calls, so
     * this one top-level call is the only entry point needed -- the
     * recursion itself drives the rest of the traversal. */
    return decode_recursive(&dctx, top);
}

} // namespace mudcbor

#endif // MUD_QCBOR_VALUE_HPP_
