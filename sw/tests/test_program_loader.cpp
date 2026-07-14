// OBP1 loader tests: a hand-assembled minimal program, every validator
// rejection path, and the checked-in sma_cross.obp (compiled offline by
// backtest_project's dsl/export.py).
#include "test_harness.hpp"
#include "../trade/program.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

using namespace ob::trade;

namespace {

uint32_t enc(uint32_t op, uint32_t arg = 0) { return (op << 24) | arg; }

// Little-endian blob builder mirroring dsl/export.py's layout.
struct Builder {
    std::vector<uint8_t> buf;

    void u16(uint16_t v) { buf.push_back(v & 0xFF); buf.push_back(v >> 8); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) buf.push_back((v >> (8 * i)) & 0xFF);
    }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
};

// "buy when close > open": PUSH_SERIES close, PUSH_SERIES open, GT, SIG_EL, HALT
Builder minimal_blob() {
    Builder b;
    b.buf.insert(b.buf.end(), {'O', 'B', 'P', '1'});
    b.u32(1);   // version
    b.u32(5);   // n_code
    b.u32(0);   // n_consts
    b.u32(0);   // n_state
    b.u32(0);   // n_params
    b.u32(0);   // n_locals
    b.u32(0);   // state_floats
    b.u32(2);   // max_stack
    b.u32(enc(OP_PUSH_SERIES, 3));
    b.u32(enc(OP_PUSH_SERIES, 0));
    b.u32(enc(OP_GT));
    b.u32(enc(OP_SIG_EL));
    b.u32(enc(OP_HALT));
    return b;
}

Program parse(const Builder& b) { return Program::parse(b.buf.data(), b.buf.size()); }

// True iff parsing throws and the message contains `frag`.
bool rejects(const Builder& b, const char* frag) {
    try {
        parse(b);
    } catch (const std::runtime_error& e) {
        return std::strstr(e.what(), frag) != nullptr;
    }
    return false;
}

void patch_header_u32(Builder& b, int field, uint32_t v) {
    // field 0 = version, 1 = n_code, ... 7 = max_stack (after 4-byte magic)
    size_t off = 4 + size_t(field) * 4;
    for (int i = 0; i < 4; ++i) b.buf[off + i] = (v >> (8 * i)) & 0xFF;
}

}  // namespace

static void test_minimal_program_parses() {
    Program p = parse(minimal_blob());
    CHECK_EQ(p.code.size(), 5u);
    CHECK_EQ(p.consts.size(), 0u);
    CHECK_EQ(p.n_locals, 0u);
    CHECK_EQ(p.max_stack, 2u);
    CHECK_EQ(p.code[0], enc(OP_PUSH_SERIES, 3));
    CHECK_EQ(p.code[4], enc(OP_HALT));
}

static void test_bad_magic() {
    Builder b = minimal_blob();
    b.buf[0] = 'X';
    CHECK(rejects(b, "magic"));
}

static void test_bad_version() {
    Builder b = minimal_blob();
    patch_header_u32(b, 0, 2);
    CHECK(rejects(b, "version"));
}

static void test_truncated() {
    Builder b = minimal_blob();
    for (size_t cut = 1; cut < b.buf.size(); cut += 7) {
        Builder t;
        t.buf.assign(b.buf.begin(), b.buf.end() - cut);
        CHECK(rejects(t, "truncated"));
    }
}

static void test_trailing_bytes() {
    Builder b = minimal_blob();
    b.buf.push_back(0);
    CHECK(rejects(b, "trailing"));
}

static void test_stack_limit() {
    Builder b = minimal_blob();
    patch_header_u32(b, 7, VM_MAX_STACK + 1);
    CHECK(rejects(b, "max_stack"));
}

static void test_empty_code() {
    Builder b;
    b.buf.insert(b.buf.end(), {'O', 'B', 'P', '1'});
    for (int i = 0; i < 8; ++i) b.u32(i == 0 ? 1 : 0);  // version 1, all else 0
    CHECK(rejects(b, "empty code"));
}

static void test_bad_opcode() {
    Builder b = minimal_blob();
    // overwrite op[2] (GT) with an out-of-range opcode
    size_t off = 4 + 8 * 4 + 2 * 4;
    uint32_t bad = enc(OP_COUNT + 3);
    for (int i = 0; i < 4; ++i) b.buf[off + i] = (bad >> (8 * i)) & 0xFF;
    CHECK(rejects(b, "bad opcode"));
}

static void test_operand_range_checks() {
    struct Case { uint32_t op, arg; const char* frag; };
    const Case cases[] = {
        {OP_PUSH_CONST, 0, "const"},      // n_consts == 0
        {OP_PUSH_PARAM, 0, "param"},      // n_params == 0
        {OP_PUSH_SERIES, 5, "series"},
        {OP_PUSH_SERIES_LAG, 5u << 16, "series"},
        {OP_PUSH_CTX, 4, "ctx"},
        {OP_LOAD, 0, "local"},            // n_locals == 0
        {OP_SMA, 0, "state slot"},        // n_state == 0
    };
    for (const Case& c : cases) {
        Builder b = minimal_blob();
        size_t off = 4 + 8 * 4;  // op[0]
        uint32_t word = enc(c.op, c.arg);
        for (int i = 0; i < 4; ++i) b.buf[off + i] = (word >> (8 * i)) & 0xFF;
        CHECK(rejects(b, c.frag));
    }
}

static void test_missing_halt() {
    Builder b = minimal_blob();
    // overwrite the final HALT with NOT (unary, keeps stack legal)
    size_t off = 4 + 8 * 4 + 4 * 4;
    uint32_t word = enc(OP_NOT);
    for (int i = 0; i < 4; ++i) b.buf[off + i] = (word >> (8 * i)) & 0xFF;
    CHECK(rejects(b, "HALT"));
}

static void test_state_slot_overrun() {
    // one SK_RING slot, cap 8 -> needs 9 floats at off 0, but state_floats = 4
    Builder b;
    b.buf.insert(b.buf.end(), {'O', 'B', 'P', '1'});
    b.u32(1);   // version
    b.u32(3);   // n_code
    b.u32(0);   // n_consts
    b.u32(1);   // n_state
    b.u32(0);   // n_params
    b.u32(0);   // n_locals
    b.u32(4);   // state_floats (too small)
    b.u32(2);   // max_stack
    b.u32(enc(OP_PUSH_SERIES, 3));
    b.u32(enc(OP_SIG_EL));
    b.u32(enc(OP_HALT));
    b.i32(SK_RING);  // kind
    b.i32(0);        // off
    b.i32(8);        // cap
    b.i32(0);        // aux
    CHECK(rejects(b, "overruns"));
}

static void test_valid_state_slot() {
    // same as above but state_floats = 9 exactly: must parse
    Builder b;
    b.buf.insert(b.buf.end(), {'O', 'B', 'P', '1'});
    b.u32(1); b.u32(3); b.u32(0); b.u32(1); b.u32(0); b.u32(0);
    b.u32(9);   // state_floats == 1 + cap
    b.u32(2);
    b.u32(enc(OP_PUSH_SERIES, 3));
    b.u32(enc(OP_SIG_EL));
    b.u32(enc(OP_HALT));
    b.i32(SK_RING); b.i32(0); b.i32(8); b.i32(0);
    Program p = parse(b);
    CHECK_EQ(p.state_kind.size(), 1u);
    CHECK_EQ(p.state_cap[0], 8);
}

static void test_disasm() {
    Program p = parse(minimal_blob());
    std::string d = p.disasm();
    CHECK(d.find("PUSH_SERIES") != std::string::npos);
    CHECK(d.find("GT") != std::string::npos);
    CHECK(d.find("SIG_EL") != std::string::npos);
    CHECK(d.find("HALT") != std::string::npos);
}

static void test_load_missing_file() {
    bool threw = false;
    try {
        Program::load("data/strategies/does_not_exist.obp");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// The checked-in artifact: strategies/sma_cross.dsl compiled by
// backtest_project (python -m dsl.export). Params: fast, slow, sl.
static void test_load_sma_cross() {
    Program p = Program::load("data/strategies/sma_cross.obp");
    CHECK(p.code.size() > 0);
    CHECK_EQ(p.param_names.size(), 3u);
    CHECK(p.param_names[0] == "fast");
    CHECK(p.param_names[1] == "slow");
    CHECK(p.param_names[2] == "sl");
    CHECK_EQ((long long)p.param_defaults[0], 20);
    CHECK_EQ((long long)p.param_defaults[1], 100);
    CHECK(p.max_stack <= VM_MAX_STACK);
    // sma over a raw series compiles to the RAW path: zero per-run storage
    // except the crossover PREV2 slots (see dsl/compiler.py ring-vs-raw).
    CHECK(p.state_kind.size() > 0);
}

void run_program_loader_tests() {
    RUN_TEST(test_minimal_program_parses);
    RUN_TEST(test_bad_magic);
    RUN_TEST(test_bad_version);
    RUN_TEST(test_truncated);
    RUN_TEST(test_trailing_bytes);
    RUN_TEST(test_stack_limit);
    RUN_TEST(test_empty_code);
    RUN_TEST(test_bad_opcode);
    RUN_TEST(test_operand_range_checks);
    RUN_TEST(test_missing_halt);
    RUN_TEST(test_state_slot_overrun);
    RUN_TEST(test_valid_state_slot);
    RUN_TEST(test_disasm);
    RUN_TEST(test_load_missing_file);
    RUN_TEST(test_load_sma_cross);
}
