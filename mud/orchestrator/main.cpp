/*
 * mud-sandbox-orchestrator: the raw-libriscv host binary driving
 * mud/guest/mud_guest.cpp's sandboxed Middleham MUD state machine.
 *
 * Per the sandboxed-godot-in-zone-server-h2o decision doc
 * (multiplayer-fabric-manuals PR #128) and the MUD prototype plan: one
 * orchestrator process per active MUD session, spawned by
 * zone-server-h2o, talking to it over an inherited fd pair (stdin for
 * commands in, stdout for turn results out -- the exact socketpair
 * framing is task #27's job; this binary only needs *a* byte stream in
 * and out, and works standalone against a real terminal for testing).
 *
 * Machine construction mirrors libriscv's own rvlinux CLI tool
 * (fabric-godot-core/modules/sandbox/thirdparty/libriscv/emulator/
 * src/main.cpp): load the ELF, machine.simulate() once to run the
 * guest's own crt/libc startup and its (trivial, immediately
 * returning) main(), then vmcall() the two exported functions by name
 * for every actual boot/step. g_mud_out_buffer's guest address is
 * resolved once via Memory::resolve_address(), the same symbol lookup
 * vmcall(const char *funcname, ...) itself uses internally.
 */

#include <libriscv/machine.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using riscv::Machine;

static constexpr int W = 8; /* riscv64 */

static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "mud-sandbox-orchestrator: cannot open guest ELF: %s\n", path.c_str());
        exit(1);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/* Length-prefixed framing on stdin/stdout: a 4-byte little-endian
 * length followed by that many CBOR bytes. Simple, matches the plan's
 * step 3 note ("message-length-prefixed vs. fixed-size reads") --
 * length-prefixed was the one actually built, since it is the one that
 * does not need every message to guess a shared fixed size. */
static bool read_frame(std::vector<uint8_t> &out) {
    uint8_t len_bytes[4];
    if (!std::cin.read((char *)len_bytes, 4)) {
        return false;
    }
    uint32_t len = (uint32_t)len_bytes[0] | ((uint32_t)len_bytes[1] << 8) |
                   ((uint32_t)len_bytes[2] << 16) | ((uint32_t)len_bytes[3] << 24);
    out.resize(len);
    if (len > 0 && !std::cin.read((char *)out.data(), len)) {
        return false;
    }
    return true;
}

static void write_frame(const uint8_t *data, size_t len) {
    uint8_t len_bytes[4] = {
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF), (uint8_t)((len >> 24) & 0xFF),
    };
    std::cout.write((const char *)len_bytes, 4);
    std::cout.write((const char *)data, (std::streamsize)len);
    std::cout.flush();
}

struct MudSandbox {
    Machine<W> machine;
    riscv::address_type<W> out_buffer_addr;

    explicit MudSandbox(const std::vector<uint8_t> &elf)
        : machine(elf, riscv::MachineOptions<W>{
              .memory_max = uint64_t(64) << 20, /* 64 MB -- this guest is small, no libgodot */
          }) {
        machine.setup_linux_syscalls(false, false); /* offline guest: no filesystem, no sockets */
        machine.setup_linux(std::vector<std::string>{"mud_guest"});
        /* Run the guest's own crt/libc startup + its trivial main() to
         * completion once, matching rvlinux's own machine.simulate()
         * step before any vmcall(). */
        try {
            machine.simulate(60'000'000ULL);
        } catch (const std::exception &e) {
            fprintf(stderr, "mud-sandbox-orchestrator: guest boot failed: %s\n", e.what());
            exit(1);
        }
        out_buffer_addr = machine.memory.resolve_address("g_mud_out_buffer");
    }

    /* Calls a guest vmcall(name, cbor_in) -> length, then copies
     * length bytes back from g_mud_out_buffer. Returns false on a
     * guest-reported error (negative return value). */
    bool call(const char *func_name, const std::vector<uint8_t> &cbor_in, std::vector<uint8_t> &cbor_out) {
        long n;
        try {
            n = (long)machine.vmcall(func_name, cbor_in, cbor_in.size());
        } catch (const std::exception &e) {
            fprintf(stderr, "mud-sandbox-orchestrator: vmcall(%s) failed: %s\n", func_name, e.what());
            return false;
        }
        if (n < 0) {
            return false;
        }
        cbor_out.resize((size_t)n);
        machine.copy_from_guest(cbor_out.data(), out_buffer_addr, (size_t)n);
        return true;
    }
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <guest-elf-path>\n", argv[0]);
        return 1;
    }
    std::vector<uint8_t> elf_bytes = read_file(argv[1]);
    MudSandbox sandbox(elf_bytes);
    fprintf(stderr, "mud-sandbox-orchestrator: guest booted, g_mud_out_buffer @ 0x%lx\n",
            (unsigned long)sandbox.out_buffer_addr);

    /* Protocol: the first frame on stdin is the mud_boot() config.
     * Every frame after that is a mud_step() command. Each gets one
     * frame back on stdout -- the guest's own MudBootAck/MudTurnResult
     * CBOR, unmodified, matching the plan's "orchestrate, do not
     * combine" rule: this binary moves bytes, it does not interpret
     * the MUD's own JSON-LD shapes. */
    std::vector<uint8_t> in_frame, out_frame;
    if (!read_frame(in_frame)) {
        fprintf(stderr, "mud-sandbox-orchestrator: no boot config on stdin, exiting\n");
        return 1;
    }
    if (!sandbox.call("mud_boot", in_frame, out_frame)) {
        fprintf(stderr, "mud-sandbox-orchestrator: mud_boot failed\n");
        return 1;
    }
    write_frame(out_frame.data(), out_frame.size());

    while (read_frame(in_frame)) {
        if (!sandbox.call("mud_step", in_frame, out_frame)) {
            fprintf(stderr, "mud-sandbox-orchestrator: mud_step failed\n");
            break;
        }
        write_frame(out_frame.data(), out_frame.size());
    }
    return 0;
}
