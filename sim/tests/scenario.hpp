// scenario.hpp — shared helpers for the focused RTL unit tests.
//
// Each test crafts a precise ITCH byte sequence with itch_build.hpp, drives it
// through the Verilated orderbook_top via RtlDriver, and asserts on the RTL
// outputs (book state, decoded tap, perf counters) — optionally cross-checking
// the self-contained reference model. A tiny assert harness keeps the tests
// dependency-free and makes each return a failure count.
#pragma once

#include "../rtl_driver.hpp"
#include "../itch_build.hpp"
#include "../reference_model.hpp"
#include "../itch_file_reader.hpp"   // split_messages / reserialize

#include <cstdio>
#include <vector>
#include <string>

namespace obsim {

struct TestCtx { int checks = 0; int fails = 0; const char* name = ""; };

#define SCHECK(ctx, cond, ...)                                                  \
    do {                                                                        \
        ++(ctx).checks;                                                         \
        if (!(cond)) { ++(ctx).fails;                                           \
            std::printf("    [FAIL] %s:%d ", __FILE__, __LINE__);               \
            std::printf(__VA_ARGS__); std::printf("\n"); }                      \
    } while (0)

// Drive a fully-built length-prefixed stream (raw ITCH) through the RTL, firing
// on_commit(idx) after each committed message. Returns committed count.
inline uint64_t run_stream(RtlDriver& drv, const std::vector<uint8_t>& stream,
                           uint64_t nmsgs,
                           const std::function<void(uint64_t)>& on_commit) {
    return drv.drive(stream, nmsgs, on_commit);
}

// Drive a stream and capture every decoded-tap pulse (type + key fields), in
// stream order. Used by the decode test to verify field extraction/byte-swap.
struct DecTap {
    uint8_t  type; bool is_bid; bool printable;
    uint32_t price; uint32_t shares; uint64_t ref; uint64_t new_ref;
};
inline std::vector<DecTap> run_capture_decode(RtlDriver& drv,
                                              const std::vector<uint8_t>& stream,
                                              uint64_t nmsgs,
                                              uint64_t cycle_cap = 5'000'000ULL) {
    auto* top = drv.top();
    std::vector<DecTap> taps;
    size_t i = 0;
    while (taps.size() < nmsgs && drv.cycles() < cycle_cap) {
        if (top->in_ready && i < stream.size()) { top->in_byte = stream[i++]; top->in_valid = 1; }
        else                                     { top->in_valid = 0; }
        drv.tick();
        if (top->dec_tap_valid) {
            taps.push_back(DecTap{top->dec_tap_type, (bool)top->dec_tap_is_bid,
                                  (bool)top->dec_tap_printable, top->dec_tap_price,
                                  top->dec_tap_shares, top->dec_tap_ref, top->dec_tap_new_ref});
        }
    }
    return taps;
}

}  // namespace obsim
