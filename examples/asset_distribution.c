/*
 * asset_distribution.c -- content-addressed asset CDN on the h2o event loop.
 *
 * Large immutable assets (a 1 GB world, a 100 MB avatar) are not fanned out and
 * are not live state. They are casync chunks in a content-addressed store, each
 * named by its hash, fetched once per client and cached forever. This handler
 * is the CDN leaf: resolve a chunk by hash, check the caller may read it, send
 * the bytes.
 *
 * The chunk store is casync (fabric-casync-central / idtxcli). The actor does
 * not hold the asset in its own state -- it is the front, not the store.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <h2o.h>

#include "gen/rebac.h"

/* ---- casync seam ---------------------------------------------------------
 *
 * The one call onto the chunk store. Back it with idtxcli's fetch/verify
 * (fabric-flow-adapters): link its library or shell out. `hash` is the
 * content address from the request path; on success `*out`/`*out_len` point at
 * the verified chunk bytes, owned by the store until `casync_release`.
 */
extern int casync_fetch_chunk(const char *hash, const uint8_t **out, size_t *out_len);
extern void casync_release(const uint8_t *chunk);

/* ---- authorization -------------------------------------------------------
 *
 * Resolve the caller's rebac relations from the request (a bearer token, a
 * signed cookie -- the cheap, validated path, not the nasty one). Returns the
 * number of relations written to `out`, or -1 to reject outright.
 */
extern int resolve_caller_relations(h2o_req_t *req, rebac_relation_t *out, size_t cap);

#define MAX_RELATIONS 32
#define HASH_MAX 128

static int reject(h2o_req_t *req, int status, const char *reason)
{
	h2o_send_error_generic(req, status, "Forbidden", reason, 0);
	return 0;
}

/* GET /chunk/{hash} */
static int on_chunk(h2o_handler_t *self, h2o_req_t *req)
{
	(void)self;
	if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")))
		return reject(req, 405, "method not allowed");

	/* Path after the registered "/chunk/" prefix is the content address. */
	const char *prefix = "/chunk/";
	size_t plen = strlen(prefix);
	if (req->path_normalized.len <= plen)
		return reject(req, 400, "missing chunk hash");
	size_t hlen = req->path_normalized.len - plen;
	if (hlen >= HASH_MAX)
		return reject(req, 400, "hash too long");
	char hash[HASH_MAX];
	memcpy(hash, req->path_normalized.base + plen, hlen);
	hash[hlen] = '\0';

	/* Cheap path: authorize the caller before touching the store. */
	rebac_relation_t rels[MAX_RELATIONS];
	int nrel = resolve_caller_relations(req, rels, MAX_RELATIONS);
	if (nrel < 0 || !rebac_check(rels, (size_t)nrel, REBAC_ACTION_READ))
		return reject(req, 403, "not permitted to read this asset");

	/* Fetch the verified chunk from the content-addressed store. */
	const uint8_t *chunk = NULL;
	size_t chunk_len = 0;
	if (casync_fetch_chunk(hash, &chunk, &chunk_len) != 0)
		return reject(req, 404, "unknown chunk");

	/* Content-addressed, so the body can never change: cache it forever. */
	static h2o_generator_t gen = {NULL, NULL};
	req->res.status = 200;
	req->res.reason = "OK";
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL,
		       H2O_STRLIT("application/octet-stream"));
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CACHE_CONTROL, NULL,
		       H2O_STRLIT("public, max-age=31536000, immutable"));
	h2o_start_response(req, &gen);
	h2o_iovec_t body = h2o_iovec_init(chunk, chunk_len);
	h2o_send(req, &body, 1, H2O_SEND_STATE_FINAL);
	casync_release(chunk);
	return 0;
}

/* Register the CDN route on a host config (see src/main.c's
 * h2o_config_register_host for where hostconf comes from). */
void asset_distribution_register(h2o_hostconf_t *hostconf)
{
	h2o_pathconf_t *pathconf =
		h2o_config_register_path(hostconf, "/chunk/", 0);
	h2o_handler_t *h = h2o_create_handler(pathconf, sizeof(*h));
	h->on_req = on_chunk;
}
