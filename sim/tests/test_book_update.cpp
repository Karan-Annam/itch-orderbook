// Price-level updates and the best-price scan.
//
// Builds a known multi-level book and drives events that specifically exercise:
//   * incremental best update on Add,
//   * best-price DOWNWARD scan when the best bid level is deleted,
//   * best-price UPWARD scan when the best ask level is emptied by Execute,
//   * a non-best deletion that must NOT move the best,
// checking best price/shares against the reference at every commit — including
// the invariant that the reported best level always has nonzero shares (this is
// the test that caught the scan handshake race, see docs/DESIGN.md).
#include "scenario.hpp"

using namespace obsim;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "book_update"};

    std::vector<uint8_t> s;
    // Bids: 100000(best), 99900, 99800.  Asks: 100100(best), 100200, 100300.
    append(s, add(1, 'B', 100, 100000, 1));
    append(s, add(2, 'B', 200,  99900, 1));
    append(s, add(3, 'B', 300,  99800, 1));
    append(s, add(4, 'S', 110, 100100, 1));
    append(s, add(5, 'S', 220, 100200, 1));
    append(s, add(6, 'S', 330, 100300, 1));
    // Delete a NON-best bid (99900): best must stay 100000.
    append(s, del(2));
    // Delete the best bid (100000): scan down to 99800 (99900 already gone).
    append(s, del(1));
    // Execute the full best ask (100100): scan up to 100200.
    append(s, exec(4, 110));
    // Partial-cancel new best bid then add a new higher bid to retest incremental.
    append(s, cancel(3, 100));            // 99800 now 200 shares
    append(s, add(7, 'B', 500, 99950, 1)); // new best bid 99950

    RefBook ref; std::vector<RefMsg> rm;
    { auto msgs = split_messages(s, 0); for (auto& m : msgs) rm.push_back(decode_body(m.body)); }

    RtlDriver drv; drv.reset(true);
    uint64_t total = rm.size();
    uint64_t committed = run_stream(drv, s, total, [&](uint64_t idx){
        ref.apply(rm[idx]);
        auto* top = drv.top();
        SCHECK(ctx, (bool)top->best_bid_valid == ref.best_bid_valid(),
               "@%llu bid_valid rtl=%d ref=%d", (unsigned long long)idx,
               (int)top->best_bid_valid, ref.best_bid_valid());
        if (ref.best_bid_valid()) {
            SCHECK(ctx, top->best_bid_price == ref.best_bid_price(),
                   "@%llu best_bid rtl=%u ref=%u", (unsigned long long)idx,
                   (unsigned)top->best_bid_price, ref.best_bid_price());
            SCHECK(ctx, top->best_bid_shares == ref.best_bid_shares(),
                   "@%llu bid_shares rtl=%u ref=%llu", (unsigned long long)idx,
                   (unsigned)top->best_bid_shares, (unsigned long long)ref.best_bid_shares());
            // safety: reported best level must have nonzero shares
            SCHECK(ctx, top->best_bid_shares != 0, "@%llu best bid has zero shares!",
                   (unsigned long long)idx);
        }
        if (ref.best_ask_valid()) {
            SCHECK(ctx, top->best_ask_price == ref.best_ask_price(),
                   "@%llu best_ask rtl=%u ref=%u", (unsigned long long)idx,
                   (unsigned)top->best_ask_price, ref.best_ask_price());
            SCHECK(ctx, top->best_ask_shares != 0, "@%llu best ask has zero shares!",
                   (unsigned long long)idx);
        }
    });
    SCHECK(ctx, committed == total, "committed %llu/%llu", (unsigned long long)committed,
           (unsigned long long)total);

    // Final expected state: best bid 99950 (500), best ask 100200 (220).
    auto* top = drv.top();
    SCHECK(ctx, top->best_bid_price == 99950 && top->best_bid_shares == 500,
           "final best_bid=%u(%u) expected 99950(500)",
           (unsigned)top->best_bid_price, (unsigned)top->best_bid_shares);
    SCHECK(ctx, top->best_ask_price == 100200 && top->best_ask_shares == 220,
           "final best_ask=%u(%u) expected 100200(220)",
           (unsigned)top->best_ask_price, (unsigned)top->best_ask_shares);
    SCHECK(ctx, top->scan_count >= 2, "expected >=2 best-price scans, got %llu",
           (unsigned long long)top->scan_count);

    std::printf("[book_update] %d checks, %d failures (scans=%llu)\n",
                ctx.checks, ctx.fails, (unsigned long long)top->scan_count);
    return ctx.fails ? 1 : 0;
}
