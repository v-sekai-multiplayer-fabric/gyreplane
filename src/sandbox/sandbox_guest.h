#ifndef SANDBOX_GUEST_H_
#define SANDBOX_GUEST_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Guest ELF loader -- RFD 0094's item 1, the host side of the minimum
 * UGC game loop. C-callable so main.c can start a guest without
 * touching C++; the implementation (sandbox_guest.cpp) links
 * thirdparty/libriscv and src/sandbox/zf_guestfs.
 *
 * Isolation contract (RFD 0092 use plane + RFD 0094's constraints):
 *   - No host filesystem: setup_linux_syscalls(filesystem=false), and
 *     every file syscall re-routes into zf_guestfs (FDB in disguise).
 *   - No sockets, ever: setup_linux_syscalls(sockets=false). Guests
 *     never reach the h2o event loop or any host networking; some of
 *     their "offline" file calls are the networked calls, hidden.
 *   - Hard limits: guest memory (memory_max), instruction budget per
 *     run, and zf_guestfs's own storage ceilings.
 */

typedef struct {
    const char *elf_path;      /* guest ELF on the host disk (CDN-fetched) */
    const char *cluster_file;  /* FDB cluster file, for zf_guestfs */
    uint32_t    z_id;
    uint64_t    memory_max;    /* guest memory ceiling, bytes */
    uint64_t    max_instructions; /* per sandbox_guest_run() call */
} sandbox_guest_config_t;

#define SANDBOX_GUEST_MEMORY_MAX_DEFAULT (512ull << 20)
#define SANDBOX_GUEST_MAX_INSTR_DEFAULT  (16ull * 1000 * 1000 * 1000)

/*
 * Spawns the dedicated guest pthread: ReBAC-gates the load (admin
 * plane, modify/owner -- see the call site's identity note), loads the
 * ELF, installs the VFS syscall layer, runs the guest to completion or
 * budget exhaustion, logs the outcome. Returns 0 if the thread
 * started, -1 otherwise. Fire-and-forget: the thread detaches, since
 * a guest's lifetime is independent of any one request.
 */
int sandbox_guest_start(const sandbox_guest_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif
