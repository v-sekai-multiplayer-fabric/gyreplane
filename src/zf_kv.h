#ifndef ZF_KV_H_
#define ZF_KV_H_

#define FDB_API_VERSION 730

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <foundationdb/fdb_c.h>

/*
 * Zonefabric key-value encoding layer for FoundationDB.
 *
 * Ported from weftspun/h2o-bench-tpcc's tpcc_kv.{c,h} pattern (prefix +
 * big-endian integer keys, packed binary values) and RFD 0002's keyspace
 * design (multiplayer-fabric-manuals/decisions/20260806-zonefabric-scaling.md,
 * "FDB keyspace design" / "Per-entity keyspace" section).
 *
 * Scope for task #7 (M3 -- ZoneTick): zf/zone/ and zf/entity/ only. The
 * batched zf/zone_state/ blob (RFD 0002's slotmap + zstd path) and the
 * zf/effect/, zf/fanout/ keyspaces (CastSpell, task H/I in RFD 0020) are
 * later milestones, not this one.
 *
 * Entity shape matches src/transport/webtransport_server.h's
 * zonetick_entity_t (3D position/velocity, no acceleration) rather than
 * FabricZone's 9-field FabricEntity or RFD 0002's 2D benchmark shape --
 * this is a placeholder pending task #10's lean-entity-packet codegen,
 * which is the actual source of truth for the wire/storage entity type.
 */

#define SS_ZF_ZONE   "zf/zone/"
#define SS_ZF_ENTITY "zf/entity/"

/* --- Key builders --- */

/* "zf/zone/{z_id}" -- z_id big-endian u32. Writes into buf, returns length. */
size_t zf_kv_zone_key(uint8_t *buf, uint32_t z_id);

/* "zf/entity/{z_id}/{e_id}" -- both big-endian u32. */
size_t zf_kv_entity_key(uint8_t *buf, uint32_t z_id, uint32_t e_id);

/* Range [begin, end) covering every "zf/entity/{z_id}/" prefix, for the
 * per-tick range scan (RFD 0002's ZoneTick: "1 range read (200 KV pairs)"). */
size_t zf_kv_entity_range_begin(uint8_t *buf, uint32_t z_id);
size_t zf_kv_entity_range_end(uint8_t *buf, uint32_t z_id);

/* --- Value structs (packed binary) --- */
#pragma pack(push, 1)

typedef struct {
    uint32_t authority_cap;
    uint32_t interest_cap;
    double   cost;
    uint32_t population;
} zf_zone_val_t;

typedef struct {
    uint32_t e_id;     /* redundant with the key, kept so a range-scanned
                           value is self-describing without re-parsing the key */
    double   cx, cy, cz;
    double   vx, vy, vz;
} zf_entity_val_t;

#pragma pack(pop)

#define ZF_ZONE_VAL_SIZE   ((int)sizeof(zf_zone_val_t))
#define ZF_ENTITY_VAL_SIZE ((int)sizeof(zf_entity_val_t))

/* --- Encode/decode (host byte order in the struct; FDB values are just
 * opaque bytes, so no wire-endianness conversion is needed here the way
 * key bytes need big-endian for lexicographic ordering) --- */

void zf_kv_encode_zone(uint8_t *buf, const zf_zone_val_t *val);
void zf_kv_decode_zone(const uint8_t *buf, int len, zf_zone_val_t *val);

void zf_kv_encode_entity(uint8_t *buf, const zf_entity_val_t *val);
void zf_kv_decode_entity(const uint8_t *buf, int len, zf_entity_val_t *val);

/* --- Big-endian integer encoding for keys (matches tpcc_kv.c) --- */
void zf_kv_encode_u32_be(uint8_t *buf, uint32_t val);
uint32_t zf_kv_decode_u32_be(const uint8_t *buf);

#endif
