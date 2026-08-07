/* Native smoke test for the_gyre domain (RFD 0085, multiplayer-fabric-
 * manuals), added alongside the DIFFERENTIAL_TEST.md record for
 * Middleham. Links mud_guest.cpp directly (MUD_GUEST_NO_MAIN, the same
 * mechanism that file's own comment documents for this purpose) and
 * drives mud_boot()/mud_step() through the whole loop: boot into the
 * Gyre, look, walk east into the Splicer's Den, look again, and
 * confirm the objective (both rooms visited) completes.
 *
 * Not run under libriscv/qemu-riscv64 here (no cross toolchain in this
 * environment) -- a native x86-64 build of the same guest source, the
 * same relationship DIFFERENTIAL_TEST.md's own Python-vs-guest run
 * has to a real riscv64 run of this file. Real coverage of the domain
 * switch and the new room table; not a substitute for a real
 * riscv64-musl + libriscv run before this ships.
 *
 * Build (needs QCBOR's sources and headers; laurencelundblade/QCBOR,
 * $QCBOR pointing at a checkout):
 *
 *   cc -std=c99 -O2 -I "$QCBOR/inc" -c "$QCBOR/src/qcbor_decode.c" \
 *     "$QCBOR/src/qcbor_encode.c" "$QCBOR/src/ieee754.c" \
 *     "$QCBOR/src/UsefulBuf.c" "$QCBOR/src/qcbor_err_to_str.c"
 *   c++ -std=c++17 -O2 -I "$QCBOR/inc" -DMUD_GUEST_NO_MAIN \
 *     gyre_smoke_test.cpp qcbor_decode.o qcbor_encode.o ieee754.o \
 *     UsefulBuf.o qcbor_err_to_str.o -o gyre_smoke_test
 *   ./gyre_smoke_test
 *
 * The .c files compile as C (QCBOR is a C library; compiling them as
 * C++ trips real strict-C++ type errors QCBOR's own C code relies on
 * C's looser conversion rules to avoid).
 */
#ifndef MUD_GUEST_NO_MAIN
#define MUD_GUEST_NO_MAIN
#endif
#include "../mud_guest.cpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace mud;

static std::vector<uint8_t> encode_cfg() {
    Value cfg = Value::object();
    cfg.set("seed", Value::integer(1));
    cfg.set("domain", Value::str("the_gyre"));
    cfg.set("max_turns", Value::integer(10));
    return mudcbor::encode(cfg);
}

static std::vector<uint8_t> encode_cmd(const std::string &command, const std::string &arg) {
    Value cmd = Value::object();
    cmd.set("command", Value::str(command));
    if (!arg.empty()) {
        Value args = Value::array();
        args.arr.push_back(Value::str(arg));
        cmd.set("args", args);
    }
    return mudcbor::encode(cmd);
}

static Value step(const std::string &command, const std::string &arg) {
    std::vector<uint8_t> cmd = encode_cmd(command, arg);
    long n = mud_step(cmd.data(), cmd.size());
    assert(n > 0);
    return mudcbor::decode(g_mud_out_buffer, (size_t)n);
}

int main() {
    std::vector<uint8_t> cfg = encode_cfg();
    long boot_n = mud_boot(cfg.data(), cfg.size());
    assert(boot_n > 0);
    printf("boot ok (%ld bytes)\n", boot_n);

    Value r1 = step("look", "");
    std::string narr1 = r1.get_str("narration");
    printf("turn 1 narration: %s\n", narr1.c_str());
    assert(narr1.find("Decanting Floor") != std::string::npos);

    Value r2 = step("go", "east");
    std::string narr2 = r2.get_str("narration");
    printf("turn 2 narration: %s\n", narr2.c_str());
    assert(narr2.find("Splicer's Den") != std::string::npos);
    assert(r2.get_str("post_room") == "splicers_den");

    Value r3 = step("look", "");
    std::string narr3 = r3.get_str("narration");
    printf("turn 3 narration: %s\n", narr3.c_str());
    assert(narr3.find("Splicer's Den") != std::string::npos);

    assert(g_session->objective_complete());
    printf("objective_complete() == true after visiting both Gyre rooms\n");

    printf("PASS\n");
    return 0;
}
