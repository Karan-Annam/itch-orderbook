// Order Replace (U) semantics.
//
// Replace = cancel the original ref + add a new ref at a new price/size on the
// SAME side. Exercises:
//   * a replace that moves an order to a new (non-best) price,
//   * a replace of the best order to a worse price -> best must scan/move,
//   * a replace to a better price -> becomes the new best,
//   * that the original ref is truly gone (a later op on it is a no-op) and the
//     new ref is live (a later op on it takes effect),
// all checked against the reference model at every commit.
#include "scenario.hpp"

using namespace obsim;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "replace"};

    std::vector<uint8_t> s;
    append(s, add(1, 'B', 100, 100000, 1));   // best bid 100000
    append(s, add(2, 'B', 200,  99900, 1));
    append(s, add(3, 'S', 150, 100100, 1));   // best ask 100100
    // Replace best bid 1 -> ref 10 at WORSE price 99800 (size 250): best -> 99900.
    append(s, replace(1, 10, 250, 99800));
    // Replace ref 2 (99900, now best) -> ref 11 at BETTER price 99950: new best.
    append(s, replace(2, 11, 120, 99950));
    // Replace best ask 3 -> ref 12 at better (lower) ask 100050.
    append(s, replace(3, 12, 90, 100050));
    // Original refs gone: executing ref 1 / 2 / 3 must be no-ops.
    append(s, exec(1, 50));
    append(s, exec(2, 50));
    append(s, exec(3, 50));
    // New ref live: delete ref 11 (current best bid 99950) -> best falls to 99800.
    append(s, del(11));

    RefBook ref; std::vector<RefMsg> rm;
    { auto msgs = split_messages(s, 0); for (auto& m : msgs) rm.push_back(decode_body(m.body)); }

    RtlDriver drv; drv.reset(true);
    uint64_t total = rm.size();
    uint64_t committed = run_stream(drv, s, total, [&](uint64_t idx){
        ref.apply(rm[idx]);
        auto* top = drv.top();
        if (ref.best_bid_valid()) {
            SCHECK(ctx, top->best_bid_price == ref.best_bid_price(),
                   "@%llu best_bid rtl=%u ref=%u", (unsigned long long)idx,
                   (unsigned)top->best_bid_price, ref.best_bid_price());
            SCHECK(ctx, top->best_bid_shares == ref.best_bid_shares(),
                   "@%llu bid_shares rtl=%u ref=%llu", (unsigned long long)idx,
                   (unsigned)top->best_bid_shares, (unsigned long long)ref.best_bid_shares());
        }
        if (ref.best_ask_valid()) {
            SCHECK(ctx, top->best_ask_price == ref.best_ask_price(),
                   "@%llu best_ask rtl=%u ref=%u", (unsigned long long)idx,
                   (unsigned)top->best_ask_price, ref.best_ask_price());
        }
        SCHECK(ctx, top->tot_bid_vol == ref.tot_bid_vol(),
               "@%llu tot_bid rtl=%llu ref=%llu", (unsigned long long)idx,
               (unsigned long long)top->tot_bid_vol, (unsigned long long)ref.tot_bid_vol());
    });
    SCHECK(ctx, committed == total, "committed %llu/%llu", (unsigned long long)committed,
           (unsigned long long)total);

    auto* top = drv.top();
    // After deleting ref 11 (99950), best bid is the replaced ref 10 at 99800/250.
    SCHECK(ctx, top->best_bid_price == 99800 && top->best_bid_shares == 250,
           "final best_bid=%u(%u) expected 99800(250)",
           (unsigned)top->best_bid_price, (unsigned)top->best_bid_shares);
    SCHECK(ctx, top->best_ask_price == 100050, "final best_ask=%u expected 100050",
           (unsigned)top->best_ask_price);
    SCHECK(ctx, top->mc_replace == 3, "replace count=%llu expected 3",
           (unsigned long long)top->mc_replace);

    std::printf("[replace] %d checks, %d failures\n", ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
