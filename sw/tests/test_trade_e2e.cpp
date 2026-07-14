// End-to-end trading session on a fixed-seed synthetic ITCH stream: exact
// golden counts (deterministic: integer prices, float32 VM, seeded RNG) plus
// structural invariants. Regenerate the goldens by running
//   build/orderbook_trade --gen 100000 --strategy data/strategies/sma_cross.obp
// after any intentional behavior change.
#include "test_harness.hpp"
#include "../trade/session.hpp"
#include "../util/itch_gen.hpp"

#include <cmath>

using namespace ob;
using namespace ob::trade;

namespace {

std::vector<uint8_t> gen_stream(size_t n) {
    GenConfig gc;                 // default seed
    gc.aggress_prob = 20;
    gc.ts_scale = 100000;
    return ItchGenerator(gc).generate(n);
}

}  // namespace

static void test_e2e_join_golden() {
    std::vector<uint8_t> bytes = gen_stream(100000);
    Program prog = Program::load("data/strategies/sma_cross.obp");
    SessionConfig sc;             // join entries, fee 0.001, equity0 1e9
    sc.bar_ns = 5'000'000'000ull;
    SessionResult r = run_session(bytes.data(), bytes.size(), prog, sc);

    CHECK_EQ(r.messages, 100004u);            // 100k + system events
    CHECK_EQ(r.bars.size(), 52);
    CHECK_EQ(r.maker_fills, 30);
    CHECK_EQ(r.taker_fills, 1);
    CHECK_EQ(r.trades.size(), 1u);
    const TradeRecT& tr = r.trades[0];
    CHECK_EQ(tr.side, 1);
    CHECK_EQ((int)tr.qty, 2001);
    CHECK_EQ((int)tr.entry_px, 499600);
    CHECK_EQ((int)tr.exit_px, 499798);
    CHECK_EQ(tr.reason, RSN_SIGNAL);
    CHECK(std::fabs(r.final_equity - 998396416.0f) < 1.0f);

    // structural invariants
    CHECK_EQ((int)r.equity.size(), r.bars.size());
    for (const FillRec& f : r.fills) CHECK(f.shares > 0);
    // long-only strategy: entries buy, exits sell
    uint64_t bought = 0, sold = 0;
    for (const FillRec& f : r.fills)
        (f.side == 'B' ? bought : sold) += f.shares;
    CHECK_EQ(bought, sold);                   // flat at the end
}

static void test_e2e_cross_mode() {
    std::vector<uint8_t> bytes = gen_stream(100000);
    Program prog = Program::load("data/strategies/sma_cross.obp");
    SessionConfig sc;
    sc.bar_ns = 5'000'000'000ull;
    sc.om.entry_cross = true;
    SessionResult r = run_session(bytes.data(), bytes.size(), prog, sc);

    CHECK_EQ(r.bars.size(), 52);              // same market, same bars
    CHECK(r.taker_fills >= 1);                // crossing entries take
    CHECK(r.trades.size() >= 1u);
    CHECK(r.final_equity < sc.om.equity0);    // paid the spread
    // crossing pays more than joining on the same stream/strategy
    Program prog2 = Program::load("data/strategies/sma_cross.obp");
    SessionConfig sj;
    sj.bar_ns = 5'000'000'000ull;
    SessionResult rj = run_session(bytes.data(), bytes.size(), prog2, sj);
    CHECK(r.final_equity <= rj.final_equity);
}

static void test_e2e_deterministic() {
    std::vector<uint8_t> bytes = gen_stream(50000);
    Program prog = Program::load("data/strategies/rsi_meanrev.obp");
    SessionConfig sc;
    sc.bar_ns = 5'000'000'000ull;
    SessionResult a = run_session(bytes.data(), bytes.size(), prog, sc);
    SessionResult b = run_session(bytes.data(), bytes.size(), prog, sc);
    CHECK_EQ(a.bars.size(), b.bars.size());
    CHECK_EQ(a.fills.size(), b.fills.size());
    CHECK_EQ(a.trades.size(), b.trades.size());
    CHECK(a.final_equity == b.final_equity);
}

void run_trade_e2e_tests() {
    RUN_TEST(test_e2e_join_golden);
    RUN_TEST(test_e2e_cross_mode);
    RUN_TEST(test_e2e_deterministic);
}
