#ifndef MUD_SESSION_H_
#define MUD_SESSION_H_

/*
 * mud_session: one mud-sandbox-orchestrator child process per active
 * MUD session, matching PR #128's "orchestrate, do not combine" rule
 * and the plan's step 2/3. Session id is client-minted (no accounts,
 * "no need for oauth" per the user's own instruction).
 *
 * Known, stated limitation for this prototype: mud_session_step()
 * blocks the calling h2o worker thread on the child process's pipe
 * I/O. A single mud_step() round trip is a handful of microseconds
 * (task #33 measures the real number), so this is acceptable for a
 * prototype under light load, but it is a real limitation, not a
 * hidden one -- a future pass would move this onto h2o's own evloop
 * fd-registration pattern (the same one webtransport_server.c already
 * uses for udp_sock/timer_sock), matching how the plan's step 3
 * originally described it.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct mud_session mud_session_t;

/* Must be called once before any other mud_session_* call.
 * orchestrator_path/guest_elf_path are real filesystem paths to the
 * built mud-sandbox-orchestrator binary and mud_guest.rv64.elf. */
void mud_session_init(const char *orchestrator_path, const char *guest_elf_path);

/* Finds an existing session by id, or spawns a new orchestrator child
 * and sends it a real mud_boot() CBOR frame (seed derived from the
 * session id's hash, objective as given) if none exists yet. Returns
 * NULL on a real spawn/boot failure. */
mud_session_t *mud_session_get_or_create(const char *session_id, const char *objective);

/* Sends one mud_step() CBOR command frame, blocks for the matching
 * response frame. *out is malloc()'d, caller frees it. Returns 0 on
 * success, -1 on a real I/O or child-crash failure. */
int mud_session_step(mud_session_t *session, const uint8_t *cmd_cbor, size_t cmd_len, uint8_t **out, size_t *out_len);

/* Terminates every live orchestrator child. Called on shutdown. */
void mud_session_close_all(void);

#endif // MUD_SESSION_H_
