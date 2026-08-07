/*
 * Guest ELF loader implementation. See sandbox_guest.h for the
 * isolation contract, zf_guestfs.h for the storage contract.
 *
 * The syscall layer here mirrors zone-guest-godot's verified rvlinux
 * boot recipe (its libriscv-fixes.patch, fixes 4-11), with one
 * deliberate divergence: instead of proxying file syscalls to the
 * host filesystem the way rvlinux does, every file syscall lands in
 * zf_guestfs -- FDB in disguise, the guest believes it is offline.
 *
 * Handler-table note: libriscv's install_syscall_handler is STATIC per
 * Machine template width, so the VFS context must come from
 * machine.get_userdata(), never from a global. One process runs one
 * zone (README's 1 process : 1 zone), and this loader runs one guest
 * thread; multiple guests per zone (RFD 0094's composition) will need
 * per-machine userdata anyway, which this already does.
 */

#include "sandbox_guest.h"
#include "zf_guestfs.h"

extern "C" {
#include "gen/rebac.h"
}

#include <libriscv/machine.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <string>
#include <vector>

static constexpr int W = 8; /* riscv64 */
using SandboxMachine = riscv::Machine<W>;

/* Per-machine context, reached via machine.get_userdata(). */
struct sandbox_ctx {
    zf_guestfs_t *fs;
    uint32_t      z_id;
};

/* Guest fd namespace: 0/1/2 are console, VFS handles start here. */
static constexpr int GFS_FD_BASE = 4;

static sandbox_ctx *ctx_of(SandboxMachine &m)
{
    return m.template get_userdata<sandbox_ctx>();
}

/* --- file syscall layer over zf_guestfs -------------------------------- */

static void sys_openat(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    /* dirfd is ignored on purpose: the VFS namespace is closed and
     * rooted, every guest path normalizes into it (zf_guestfs.c's
     * gfs_normalize_path), so directory-relative resolution collapses
     * to root-relative. AT_FDCWD arrives here too and needs nothing. */
    const auto g_path = m.sysarg(1);
    const int flags = m.template sysarg<int>(2);
    std::string path = m.memory.memstring(g_path);

    const bool creat = (flags & 0100) != 0; /* O_CREAT, riscv64 ABI */
    int h = zf_guestfs_open(ctx->fs, path.c_str(), creat);
    m.set_result(h < 0 ? h : h + GFS_FD_BASE);
}

static void sys_close(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const int vfd = m.template sysarg<int>(0);
    if (vfd < GFS_FD_BASE) { m.set_result(0); return; } /* console fds */
    m.set_result(zf_guestfs_close(ctx->fs, vfd - GFS_FD_BASE));
}

static void sys_read(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const int vfd = m.template sysarg<int>(0);
    const auto g_buf = m.sysarg(1);
    const uint64_t len = m.sysarg(2);
    if (vfd < GFS_FD_BASE) { m.set_result(0); return; } /* console: EOF */

    std::vector<uint8_t> tmp(len);
    int64_t n = zf_guestfs_read(ctx->fs, vfd - GFS_FD_BASE, tmp.data(), len);
    if (n > 0) m.copy_to_guest(g_buf, tmp.data(), (size_t)n);
    m.set_result(n);
}

static void sys_write(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const int vfd = m.template sysarg<int>(0);
    const auto g_buf = m.sysarg(1);
    const uint64_t len = m.sysarg(2);

    if (len == 0) { m.set_result(0); return; }
    std::vector<uint8_t> tmp(len);
    m.copy_from_guest(tmp.data(), g_buf, len);

    if (vfd >= GFS_FD_BASE) {
        m.set_result(zf_guestfs_write(ctx->fs, vfd - GFS_FD_BASE, tmp.data(), len));
        return;
    }
    /* Console: guest stdout/stderr go to the host log, zone-tagged.
     * This is observability output, not a capability. */
    fprintf(stderr, "zone %u guest: %.*s", ctx->z_id, (int)len, (const char *)tmp.data());
    m.set_result((int64_t)len);
}

static void sys_writev(SandboxMachine &m)
{
    /* Decompose into sys_write per iov: simplest correct form, and
     * writev's atomicity guarantee is meaningless over a private
     * in-memory buffer. */
    auto *ctx = ctx_of(m);
    const int vfd = m.template sysarg<int>(0);
    const auto g_iov = m.sysarg(1);
    const int iovcnt = m.template sysarg<int>(2);
    if (iovcnt < 0 || iovcnt > 64) { m.set_result(-EINVAL); return; }

    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint64_t iov[2]; /* guest struct iovec, riscv64: base, len */
        m.copy_from_guest(iov, g_iov + (uint64_t)i * 16, 16);
        if (iov[1] == 0) continue;
        std::vector<uint8_t> tmp(iov[1]);
        m.copy_from_guest(tmp.data(), iov[0], iov[1]);
        if (vfd >= GFS_FD_BASE) {
            int64_t n = zf_guestfs_write(ctx->fs, vfd - GFS_FD_BASE, tmp.data(), iov[1]);
            if (n < 0) { m.set_result(total > 0 ? total : n); return; }
            total += n;
            if ((uint64_t)n < iov[1]) break;
        } else {
            fprintf(stderr, "zone %u guest: %.*s", ctx->z_id, (int)iov[1],
                    (const char *)tmp.data());
            total += (int64_t)iov[1];
        }
    }
    m.set_result(total);
}

static void sys_lseek(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const int vfd = m.template sysarg<int>(0);
    const int64_t off = (int64_t)m.sysarg(1);
    const int whence = m.template sysarg<int>(2);
    if (vfd < GFS_FD_BASE) { m.set_result(-ESPIPE); return; }
    m.set_result(zf_guestfs_lseek(ctx->fs, vfd - GFS_FD_BASE, off, whence));
}

/* Minimal stat: Godot's boot path checks existence and size (its
 * newfstatat on godot.log before creating it, per the zone-guest-godot
 * strace). st_size at offset 48, st_mode at 16 (riscv64 struct stat);
 * everything else zeros. */
static void fill_stat(SandboxMachine &m, uint64_t g_statbuf, int64_t size)
{
    uint8_t st[128];
    memset(st, 0, sizeof(st));
    const uint32_t mode = 0100644; /* S_IFREG | 0644 */
    memcpy(st + 16, &mode, 4);
    memcpy(st + 48, &size, 8);
    m.copy_to_guest(g_statbuf, st, sizeof(st));
}

static void sys_fstat(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const int vfd = m.template sysarg<int>(0);
    const auto g_statbuf = m.sysarg(1);
    int64_t size = 0;
    if (vfd >= GFS_FD_BASE) {
        size = zf_guestfs_size(ctx->fs, vfd - GFS_FD_BASE);
        if (size < 0) { m.set_result(size); return; }
    }
    fill_stat(m, g_statbuf, size);
    m.set_result(0);
}

static void sys_newfstatat(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const auto g_path = m.sysarg(1);
    const auto g_statbuf = m.sysarg(2);
    std::string path = m.memory.memstring(g_path);

    int64_t size = zf_guestfs_stat_size(ctx->fs, path.c_str());
    if (size < 0) { m.set_result(size); return; }
    fill_stat(m, g_statbuf, size);
    m.set_result(0);
}

static void sys_mkdirat(SandboxMachine &m)
{
    /* Directories are implicit in a flat key namespace; creation
     * always succeeds. Matches the guest's mkdir -p idiom (Godot's
     * make_dir_recursive) without tracking empty directories. */
    m.set_result(0);
}

static void sys_unlinkat(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const auto g_path = m.sysarg(1);
    std::string path = m.memory.memstring(g_path);
    m.set_result(zf_guestfs_unlink(ctx->fs, path.c_str()));
}

/* --- thread body ------------------------------------------------------- */

struct guest_thread_arg {
    sandbox_guest_config_t cfg;
    std::string            elf_path;
    std::string            cluster_file;
};

static void *guest_thread_main(void *varg)
{
    auto *arg = static_cast<guest_thread_arg *>(varg);
    const uint32_t z_id = arg->cfg.z_id;

    /*
     * Admin-plane gate (RFD 0092/0094): loading a guest is a MODIFY
     * action, owner-only. Identity is the documented gap (README:
     * TLS cert/key still NULL/NULL), so until a real subject exists,
     * the claim is the process operator's own OWNER relation -- the
     * operator started this binary with -g, which IS ownership of the
     * process. The rebac_check call is real and stays on this path so
     * the wiring never needs to move when identity lands.
     */
    const rebac_relation_t operator_claim[] = { REBAC_RELATION_OWNER };
    if (!rebac_check(operator_claim, 1, REBAC_ACTION_MODIFY)) {
        fprintf(stderr, "zone %u: guest load DENIED by rebac_check\n", z_id);
        delete arg;
        return nullptr;
    }

    std::ifstream f(arg->elf_path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "zone %u: guest ELF unreadable: %s\n", z_id,
                arg->elf_path.c_str());
        delete arg;
        return nullptr;
    }
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    zf_guestfs_limits_t limits = {
        ZF_GUESTFS_MAX_FILE_BYTES_DEFAULT,
        ZF_GUESTFS_MAX_TOTAL_BYTES_DEFAULT,
        ZF_GUESTFS_MAX_OPEN_DEFAULT,
        ZF_GUESTFS_MAX_PATH_DEFAULT,
    };
    zf_guestfs_t *fs = zf_guestfs_create(arg->cluster_file.c_str(), z_id, &limits);
    if (!fs) {
        fprintf(stderr, "zone %u: zf_guestfs_create failed\n", z_id);
        delete arg;
        return nullptr;
    }

    try {
        riscv::MachineOptions<W> options;
        options.memory_max = arg->cfg.memory_max;
        SandboxMachine machine{elf, options};

        sandbox_ctx ctx{fs, z_id};
        machine.set_userdata(&ctx);

        /* Linux syscall base WITHOUT host filesystem and WITHOUT
         * sockets -- the two false arguments are the isolation
         * contract, then the VFS layer overrides the file syscalls. */
        machine.setup_linux_syscalls(false, false);
        machine.setup_posix_threads();
        machine.setup_linux({"guest", "--headless"}, {"LC_ALL=C", "HOME=/"});

        SandboxMachine::install_syscall_handler(34, sys_mkdirat);
        SandboxMachine::install_syscall_handler(35, sys_unlinkat);
        SandboxMachine::install_syscall_handler(56, sys_openat);
        SandboxMachine::install_syscall_handler(57, sys_close);
        SandboxMachine::install_syscall_handler(62, sys_lseek);
        SandboxMachine::install_syscall_handler(63, sys_read);
        SandboxMachine::install_syscall_handler(64, sys_write);
        SandboxMachine::install_syscall_handler(66, sys_writev);
        SandboxMachine::install_syscall_handler(79, sys_newfstatat);
        SandboxMachine::install_syscall_handler(80, sys_fstat);

        fprintf(stderr, "zone %u: guest booting (%zu byte ELF, %llu MB mem, %llu Minstr budget)\n",
                z_id, elf.size(),
                (unsigned long long)(arg->cfg.memory_max >> 20),
                (unsigned long long)(arg->cfg.max_instructions / 1000000));

        machine.simulate(arg->cfg.max_instructions);

        fprintf(stderr, "zone %u: guest exited, status %d, %llu instructions\n",
                z_id, machine.return_value<int>(),
                (unsigned long long)machine.instruction_counter());
    } catch (const std::exception &e) {
        fprintf(stderr, "zone %u: guest fault: %s\n", z_id, e.what());
    }

    zf_guestfs_destroy(fs);
    delete arg;
    return nullptr;
}

extern "C" int sandbox_guest_start(const sandbox_guest_config_t *cfg)
{
    auto *arg = new guest_thread_arg{};
    arg->cfg = *cfg;
    arg->elf_path = cfg->elf_path;
    arg->cluster_file = cfg->cluster_file;
    if (arg->cfg.memory_max == 0)
        arg->cfg.memory_max = SANDBOX_GUEST_MEMORY_MAX_DEFAULT;
    if (arg->cfg.max_instructions == 0)
        arg->cfg.max_instructions = SANDBOX_GUEST_MAX_INSTR_DEFAULT;

    pthread_t tid;
    if (pthread_create(&tid, nullptr, guest_thread_main, arg) != 0) {
        delete arg;
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
