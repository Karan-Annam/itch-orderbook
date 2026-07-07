// test_order_ref_table.cpp — RTL hash table insert/lookup/delete + collisions.
//
// The hardware hash is an XOR-fold of the 64-bit ref into 16 bits:
//   idx = ref[63:48]^[47:32]^[31:16]^[15:0]
// We deliberately construct many distinct refs that all fold to the SAME index
// (ref = {0,0,a,a^H} folds to H for any a), forcing a long linear-probe chain,
// then exercise lookup (Execute) and delete on every one. Correct book updates
// prove insert/lookup/delete + collision handling; the probe-distribution perf
// counters must show the collisions actually occurred.
#include "scenario.hpp"

using namespace obsim;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "order_ref_table"};

    const uint16_t H = 0x1234;
    const uint32_t PX = 200000;          // all rest at one price level
    const int   N = 24;

    std::vector<uint64_t> refs;
    std::vector<uint8_t> s;
    for (int a = 1; a <= N; ++a) {
        uint64_t lo = uint16_t(a) ^ H;
        uint64_t ref = (uint64_t(uint16_t(a)) << 16) | lo;   // folds to H
        refs.push_back(ref);
        append(s, add(ref, 'B', 10, PX, 1));                 // each adds 10 shares
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
