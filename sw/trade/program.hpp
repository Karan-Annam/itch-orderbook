// OBP1 strategy bytecode: loader, validator, disassembler.
//
// Strategies are written in the backtest DSL and compiled offline by
// backtest_project (python -m dsl.export foo.dsl foo.obp); this loader is the
// only thing the trading engine needs at runtime — no Python dependency.
//
// The opcode numbering mirrors backtest_project/engine/cpu/opcodes.h, which is
// generated from dsl/opcodes.py (the single source of truth). If the DSL grows
// an opcode, regenerate there and extend the enum + validator here.
//
// OBP1 layout (little-endian, no padding):
//   header   9 x u32   magic 'OBP1', version, n_code, n_consts, n_state,
//                      n_params, n_locals, state_floats, max_stack
//   code     u32[n_code]           (op << 24 | arg)
//   consts   f32[n_consts]
//   state    i32[n_state] x 4      kind, off, cap, aux (four arrays in a row)
//   defaults f32[n_params]
//   names    n_params x { u16 len, utf-8 bytes }
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ob::trade {

enum Op : uint32_t {
    OP_PUSH_CONST = 0, OP_PUSH_PARAM, OP_PUSH_SERIES, OP_PUSH_SERIES_LAG,
    OP_PUSH_CTX, OP_LOAD, OP_STORE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_NEG,
    OP_ABS, OP_MIN2, OP_MAX2, OP_SQRT, OP_LOG,
    OP_GT, OP_LT, OP_GE, OP_LE, OP_EQ, OP_NE,
    OP_AND, OP_OR, OP_NOT,
    OP_SMA, OP_EMA, OP_RSI, OP_ATR, OP_HIGHEST, OP_LOWEST, OP_STDDEV,
    OP_DELAY, OP_CROSSOVER, OP_CROSSUNDER,
    OP_SMA_RAW, OP_HIGHEST_RAW, OP_LOWEST_RAW, OP_STDDEV_RAW, OP_DELAY_RAW,
    OP_SIG_EL, OP_SIG_XL, OP_SIG_ES, OP_SIG_XS,
    OP_SET_STOP, OP_SET_TP, OP_SET_TRAIL, OP_SET_SIZE,
    OP_HALT,
    OP_COUNT,
};

const char* op_name(uint32_t op);

// State slot kinds (state_kind[] values) and their header sizes in floats.
enum StateKind : int32_t {
    SK_EMA = 0,      // [count, value]
    SK_WILDER1 = 1,  // atr: [count, value]
    SK_WILDER2 = 2,  // rsi: [count, prev_x, avg_gain, avg_loss]
    SK_RING = 3,     // [count, buf...] (capacity = state_cap)
    SK_PREV2 = 4,    // crossover: [count, prev_a, prev_b]
    SK_RAW = 5,      // no storage; cap + aux(sid<<16|lag) describe a window
};

constexpr uint32_t VM_MAX_STACK = 32;
constexpr uint32_t VM_MAX_CODE = 6144;
constexpr uint32_t VM_MAX_STATE_FLOATS = 8192;
constexpr uint32_t VM_MAX_WINDOW = 4096;
constexpr uint32_t N_SERIES = 5;   // open high low close volume
constexpr uint32_t N_CTX = 4;      // bar_index position entry_price equity

struct Program {
    std::vector<uint32_t> code;
    std::vector<float>    consts;
    std::vector<int32_t>  state_kind, state_off, state_cap, state_aux;
    std::vector<float>    param_defaults;
    std::vector<std::string> param_names;
    uint32_t n_locals = 0;
    uint32_t state_floats = 0;
    uint32_t max_stack = 0;

    // Parse + validate; throws std::runtime_error with a specific message on
    // any malformed input (bad magic/version, truncation, out-of-range opcode
    // or operand, state offsets past state_floats, stack over VM_MAX_STACK).
    static Program parse(const uint8_t* data, size_t len);
    static Program load(const std::string& path);

    std::string disasm() const;   // one op per line, for debugging/tests
};

}  // namespace ob::trade
