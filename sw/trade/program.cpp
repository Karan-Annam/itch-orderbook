#include "program.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ob::trade {

namespace {

const char* const OP_NAMES[OP_COUNT] = {
    "PUSH_CONST", "PUSH_PARAM", "PUSH_SERIES", "PUSH_SERIES_LAG",
    "PUSH_CTX", "LOAD", "STORE",
    "ADD", "SUB", "MUL", "DIV", "NEG",
    "ABS", "MIN2", "MAX2", "SQRT", "LOG",
    "GT", "LT", "GE", "LE", "EQ", "NE",
    "AND", "OR", "NOT",
    "SMA", "EMA", "RSI", "ATR", "HIGHEST", "LOWEST", "STDDEV",
    "DELAY", "CROSSOVER", "CROSSUNDER",
    "SMA_RAW", "HIGHEST_RAW", "LOWEST_RAW", "STDDEV_RAW", "DELAY_RAW",
    "SIG_EL", "SIG_XL", "SIG_ES", "SIG_XS",
    "SET_STOP", "SET_TP", "SET_TRAIL", "SET_SIZE",
    "HALT",
};

// Sequential little-endian reader with hard bounds checks.
struct Reader {
    const uint8_t* p;
    size_t left;

    void need(size_t n, const char* what) {
        if (left < n)
            throw std::runtime_error(std::string("obp1: truncated at ") + what);
    }
    uint32_t u32(const char* what) {
        need(4, what);
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4; left -= 4;
        return v;
    }
    uint16_t u16(const char* what) {
        need(2, what);
        uint16_t v;
        std::memcpy(&v, p, 2);
        p += 2; left -= 2;
        return v;
    }
    template <typename T>
    std::vector<T> array(uint32_t n, const char* what) {
        static_assert(sizeof(T) == 4, "OBP1 arrays are 4-byte elements");
        need(size_t(n) * 4, what);
        std::vector<T> v(n);
        std::memcpy(v.data(), p, size_t(n) * 4);
        p += size_t(n) * 4; left -= size_t(n) * 4;
        return v;
    }
};

[[noreturn]] void fail(const char* fmt, uint32_t a, uint32_t b) {
    char buf[128];
    std::snprintf(buf, sizeof buf, fmt, (unsigned)a, (unsigned)b);
    throw std::runtime_error(std::string("obp1: ") + buf);
}

// Floats a state slot needs in the state block (0 for SK_RAW).
uint32_t state_size(int32_t kind, int32_t cap) {
    switch (kind) {
        case SK_EMA:     return 2;
        case SK_WILDER1: return 2;
        case SK_WILDER2: return 4;
        case SK_RING:    return 1 + uint32_t(cap);
        case SK_PREV2:   return 3;
        case SK_RAW:     return 0;
        default:         fail("bad state kind %u (cap %u)", uint32_t(kind), uint32_t(cap));
    }
}

}  // namespace

const char* op_name(uint32_t op) {
    return op < OP_COUNT ? OP_NAMES[op] : "??";
}

Program Program::parse(const uint8_t* data, size_t len) {
    Reader r{data, len};

    r.need(4, "magic");
    if (std::memcmp(r.p, "OBP1", 4) != 0)
        throw std::runtime_error("obp1: bad magic");
    r.p += 4; r.left -= 4;

    uint32_t version = r.u32("version");
    if (version != 1)
        fail("unsupported version %u", version, 0);

    uint32_t n_code   = r.u32("n_code");
    uint32_t n_consts = r.u32("n_consts");
    uint32_t n_state  = r.u32("n_state");
    uint32_t n_params = r.u32("n_params");

    Program prog;
    prog.n_locals     = r.u32("n_locals");
    prog.state_floats = r.u32("state_floats");
    prog.max_stack    = r.u32("max_stack");

    if (prog.max_stack > VM_MAX_STACK)
        fail("max_stack %u exceeds VM limit %u", prog.max_stack, VM_MAX_STACK);
    if (n_code == 0)
        throw std::runtime_error("obp1: empty code");

    prog.code           = r.array<uint32_t>(n_code, "code");
    prog.consts         = r.array<float>(n_consts, "consts");
    prog.state_kind     = r.array<int32_t>(n_state, "state_kind");
    prog.state_off      = r.array<int32_t>(n_state, "state_off");
    prog.state_cap      = r.array<int32_t>(n_state, "state_cap");
    prog.state_aux      = r.array<int32_t>(n_state, "state_aux");
    prog.param_defaults = r.array<float>(n_params, "param_defaults");

    for (uint32_t i = 0; i < n_params; ++i) {
        uint16_t ln = r.u16("param name len");
        r.need(ln, "param name");
        prog.param_names.emplace_back(reinterpret_cast<const char*>(r.p), ln);
        r.p += ln; r.left -= ln;
    }
    if (r.left != 0)
        fail("%u trailing bytes", uint32_t(r.left), 0);

    // ---- semantic validation --------------------------------------------
    for (uint32_t i = 0; i < n_state; ++i) {
        uint32_t size = state_size(prog.state_kind[i], prog.state_cap[i]);
        if (prog.state_off[i] < 0 ||
            uint32_t(prog.state_off[i]) + size > prog.state_floats)
            fail("state slot %u overruns state_floats %u", i, prog.state_floats);
    }

    for (uint32_t i = 0; i < n_code; ++i) {
        uint32_t op = prog.code[i] >> 24, arg = prog.code[i] & 0xFFFFFF;
        switch (op) {
            case OP_PUSH_CONST:
                if (arg >= n_consts) fail("op %u: const %u out of range", i, arg);
                break;
            case OP_PUSH_PARAM:
                if (arg >= n_params) fail("op %u: param %u out of range", i, arg);
                break;
            case OP_PUSH_SERIES:
                if (arg >= N_SERIES) fail("op %u: series %u out of range", i, arg);
                break;
            case OP_PUSH_SERIES_LAG:
                if ((arg >> 16) >= N_SERIES)
                    fail("op %u: lag series %u out of range", i, arg >> 16);
                break;
            case OP_PUSH_CTX:
                if (arg >= N_CTX) fail("op %u: ctx %u out of range", i, arg);
                break;
            case OP_LOAD: case OP_STORE:
                if (arg >= prog.n_locals) fail("op %u: local %u out of range", i, arg);
                break;
            case OP_SMA: case OP_EMA: case OP_RSI: case OP_ATR:
            case OP_HIGHEST: case OP_LOWEST: case OP_STDDEV:
            case OP_DELAY: case OP_CROSSOVER: case OP_CROSSUNDER:
            case OP_SMA_RAW: case OP_HIGHEST_RAW: case OP_LOWEST_RAW:
            case OP_STDDEV_RAW: case OP_DELAY_RAW:
                if (arg >= n_state) fail("op %u: state slot %u out of range", i, arg);
                break;
            default:
                if (op >= OP_COUNT) fail("op %u: bad opcode %u", i, op);
                break;  // arithmetic/logic/sinks/HALT take no operand
        }
    }
    if (prog.code.back() >> 24 != OP_HALT)
        throw std::runtime_error("obp1: code does not end in HALT");

    return prog;
}

Program Program::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        throw std::runtime_error("obp1: cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz > 0 ? size_t(sz) : 0);
    size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size())
        throw std::runtime_error("obp1: short read on " + path);
    return parse(buf.data(), buf.size());
}

std::string Program::disasm() const {
    std::string out;
    char line[96];
    for (size_t i = 0; i < code.size(); ++i) {
        uint32_t op = code[i] >> 24, arg = code[i] & 0xFFFFFF;
        std::snprintf(line, sizeof line, "%4zu  %-16s %u\n", i, op_name(op), arg);
        out += line;
    }
    return out;
}

}  // namespace ob::trade
