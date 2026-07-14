// test_order_ref_table.cpp — RTL hash table insert/lookup/delete + collisions.
//
// The hardware hash is a Fibonacci-style multiplicative hash (mirrored below —
// keep in sync with ob_pkg::hash_ref). Algebraic collision construction isn't
// practical against a multiply, so we SEARCH for refs that hash to one index,
// forcing a long linear-probe chain, then exercise lookup (Execute) and delete
// on every one. Correct book updates prove insert/lookup/delete + collision
// handling; the probe-distribution perf counters must show the collisions
// actually occurred.
#include "scenario.hpp"

using namespace obsim;

// mirror of ob_pkg::hash_ref (sim config: HASH_W=16)
static uint16_t hw_hash(uint64_t r) {
    uint32_t x = uint32_t(r) ^ uint32_t(r >> 32);
    uint32_t y = (x & 0x1FFFFFF) ^ ((x >> 25) << 18);
    uint32_t p = (y * 0x13C6EF5u) & 0x1FFFFFF;
    return uint16_t(p >> (25 - 16));
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "order_ref_table"};

    const uint32_t PX = 200000;          // all rest at one price level
    const int   N = 24;

    std::vector<uint64_t> refs;
    std::vector<uint8_t> s;
    const uint16_t H = hw_hash(1);       // target slot: wherever ref 1 lands
    for (uint64_t r = 1; refs.size() < size_t(N); ++r) {
        if (hw_hash(r) != H) continue;
        refs.push_back(r);
        append(s, add(r, 'B', 10, PX, 1));                   // each adds 10 shares
    }
    // Execute half by 10 (removes them), delete the rest.
    for (int i = 0; i < N; ++i) {
        if (i % 2 == 0) append(s, exec(refs[i], 10));
        else            append(s, del(refs[i]));
    }

    RefBook ref;
    std::vector<RefMsg> rm;
    { auto msgs = split_messages(s, 0); for (auto& m : msgs) rm.push_back(decode_body(m.body)); }

    RtlDriver drv; drv.reset(true);
    uint64_t total = rm.size();
    uint64_t committed = run_stream(drv, s, total, [&](uint64_t idx) {
        ref.apply(rm[idx]);
        auto* top = drv.top();
        // After each op the RTL level total must match the reference.
        SCHECK(ctx, drv.level_shares(true, PX) == ref.level_shares(true, PX),
               "@%llu level shares rtl=%u ref=%llu", (unsigned long long)idx,
               drv.level_shares(true, PX), (unsigned long long)ref.level_shares(true, PX));
    });
    SCHECK(ctx, committed == total, "committed %llu/%llu", (unsigned long long)committed,
           (unsigned long long)total);

    auto* top = drv.top();
    // All orders removed -> level empty, side empty.
    SCHECK(ctx, drv.level_shares(true, PX) == 0, "final level shares=%u",
           drv.level_shares(true, PX));
    SCHECK(ctx, !top->best_bid_valid, "best_bid should be invalid after all removed");
    // Collisions must have occurred: with N refs folding to one index, the
    // probe-2 and probe->2 counters must be nonzero.
    SCHECK(ctx, top->hash_probe_2 + top->hash_probe_gt2 > 0,
           "expected collisions: probe2=%llu probe_gt2=%llu",
           (unsigned long long)top->hash_probe_2, (unsigned long long)top->hash_probe_gt2);

    std::printf("[order_ref_table] %d checks, %d failures (probe1=%llu probe2=%llu probe_gt2=%llu)\n",
                ctx.checks, ctx.fails, (unsigned long long)top->hash_probe_1,
                (unsigned long long)top->hash_probe_2, (unsigned long long)top->hash_probe_gt2);
    return ctx.fails ? 1 : 0;
}
