// OrderManager tests: signal arbitration parity with the batch broker
// (vm_core.h's signal block), entry modes, protective exits on trade prints,
// fee/PnL arithmetic, and end-of-stream force close.
#include "test_harness.hpp"
#include "../book/order_book.hpp"
#include "../trade/fill_engine.hpp"
#include "../trade/order_manager.hpp"

#include <cmath>
#include <memory>

using namespace ob;
using namespace ob::trade;

namespace {

constexpr uint16_t LOC = 1;

Signals sig_none() { return Signals{}; }
Signals sig_el() { Signals s; s.el = true; return s; }
Signals sig_es() { Signals s; s.es = true; return s; }
Signals sig_xl() { Signals s; s.xl = true; return s; }
Signals sig_xs() { Signals s; s.xs = true; return s; }

struct OmSim {
    BookEngine eng{1 << 16, 1 << 12};
    FillEngine fe{eng, LOC};
    OmConfig cfg;
    std::unique_ptr<OrderManager> om;
    BarSeries bars;
    uint64_t ts = 1000;
    int t = 0;

    explicit OmSim(OmConfig c = OmConfig{}) : cfg(c) {
        om = std::make_unique<OrderManager>(fe, eng, LOC, cfg);
    }

    void step(DecodedMessage m) {
        m.timestamp = ++ts;
        fe.on_market_message(m);
        BookEngine::ExecResult er;
        eng.apply(m, &er);
        om->poll_fills();
        if (er.contributes && er.locate == LOC)
            om->on_trade_print(er.price, m.timestamp);
    }
    void add(uint64_t ref, char side, uint32_t px, uint32_t sh) {
        DecodedMessage m;
        m.type = MsgType::AddOrder;
        m.order_ref = ref; m.side = side; m.price = px; m.shares = sh;
        m.stock_locate = LOC;
        step(m);
    }
    void exec(uint64_t ref, uint32_t sh) {
        DecodedMessage m;
        m.type = MsgType::OrderExecuted;
        m.order_ref = ref; m.shares = sh; m.stock_locate = LOC;
        step(m);
    }

    // Close a bar at `close` and hand the given signals to the manager.
    void bar(float close, const Signals& sig) {
        bars.o.push_back(close); bars.h.push_back(close);
        bars.l.push_back(close); bars.c.push_back(close);
        bars.v.push_back(0.0f);
        om->on_bar(bars, t++, sig, ++ts);
        om->poll_fills();
    }

    const OpenOrder* working() {
        const auto& oo = fe.open_orders();
        return oo.empty() ? nullptr : &oo.front();
    }
};

// Standard two-sided book: bid 5000 x 100 (ref 10), ask 5010 x 100 (ref 11).
OmSim make_two_sided(OmConfig cfg = OmConfig{}) {
    OmSim s(cfg);
    s.add(10, 'B', 5000, 100);
    s.add(11, 'S', 5010, 100);
    return s;
}

}  // namespace

static void test_flat_entry_join_rests_at_touch() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;   // 20 shares at px 5000
    OmSim s = make_two_sided(cfg);
    s.bar(5005, sig_el());
    const OpenOrder* o = s.working();
    CHECK(o != nullptr);
    CHECK_EQ((int)o->side, (int)'B');
    CHECK_EQ(o->price, 5000u);            // joined best bid
    CHECK_EQ(o->remaining, 20u);          // floor(100000 / 5000)
    CHECK_EQ(o->queue_ahead, 100u);       // behind the displayed level
    CHECK_EQ(s.om->position(), 0);        // not filled yet
}

static void test_conflicting_entries_cancel_out() {
    OmSim s = make_two_sided();
    Signals both;
    both.el = both.es = true;
    s.bar(5005, both);
    CHECK(s.working() == nullptr);
    CHECK_EQ(s.om->position(), 0);
}

static void test_no_flip_while_positioned() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    cfg.entry_cross = true;
    OmSim s = make_two_sided(cfg);
    s.bar(5005, sig_el());                // crossing entry fills at 5010
    CHECK_EQ(s.om->position(), 1);
    s.bar(5005, sig_es());                // short entry must be ignored
    CHECK_EQ(s.om->position(), 1);
    CHECK(s.working() == nullptr);
    s.bar(5005, sig_xs());                // exit_short on a long: ignored too
    CHECK_EQ(s.om->position(), 1);
}

static void test_cross_entry_fills_and_signal_exit() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;   // floor(100000/5012) = 19 shares
    cfg.entry_cross = true;
    cfg.fee = 0.0f;
    OmSim s = make_two_sided(cfg);
    s.bar(5005, sig_el());
    CHECK_EQ(s.om->position(), 1);
    CHECK_EQ(s.om->fills().size(), 1u);
    CHECK_EQ(s.om->fills()[0].price, 5010u);   // swept the ask
    CHECK_EQ(s.om->fills()[0].shares, 19u);
    s.bar(5005, sig_xl());                // crossing exit into the bid
    CHECK_EQ(s.om->position(), 0);
    CHECK_EQ(s.om->trades().size(), 1u);
    const TradeRecT& tr = s.om->trades()[0];
    CHECK_EQ(tr.side, 1);
    CHECK_EQ(tr.reason, RSN_SIGNAL);
    CHECK_EQ((int)tr.qty, 19);
    CHECK_EQ((int)tr.entry_px, 5010);
    CHECK_EQ((int)tr.exit_px, 5000);
    CHECK_EQ((int)tr.pnl, 19 * (5000 - 5010));  // no fees
}

static void test_fee_arithmetic() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    cfg.entry_cross = true;
    cfg.fee = 0.001f;
    OmSim s = make_two_sided(cfg);
    s.bar(5005, sig_el());
    s.bar(5005, sig_xl());
    CHECK_EQ(s.om->trades().size(), 1u);
    const TradeRecT& tr = s.om->trades()[0];
    float entry_notional = 19.0f * 5010.0f;
    float exit_notional = 19.0f * 5000.0f;
    float fees = 0.001f * (entry_notional + exit_notional);
    float want = (exit_notional - entry_notional) - fees;
    CHECK(std::fabs(tr.pnl - want) < 0.01f);
    // cash round-trips: equity0 + pnl
    CHECK(std::fabs(s.om->cash() - (cfg.equity0 + want)) < 0.01f);
}

static void test_stop_on_trade_print() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    cfg.entry_cross = true;
    OmSim s = make_two_sided(cfg);
    Signals el = sig_el();
    el.stop = 0.01f;                      // 1% stop
    s.bar(5005, el);                      // long 19 @ 5010; stop at 4959.9
    CHECK_EQ(s.om->position(), 1);
    s.add(20, 'B', 4950, 50);             // deeper bid for the exit to hit
    s.exec(10, 10);                       // print at 5000: above stop, no exit
    CHECK_EQ(s.om->position(), 1);
    // print at 4950 (below trigger): stop fires, exit crosses into the bids
    s.exec(20, 10);
    CHECK_EQ(s.om->position(), 0);
    CHECK_EQ(s.om->trades().size(), 1u);
    CHECK_EQ(s.om->trades()[0].reason, RSN_STOP);
}

static void test_trail_advances_with_prints() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    cfg.entry_cross = true;
    OmSim s = make_two_sided(cfg);
    Signals el = sig_el();
    el.trail = 0.01f;
    s.bar(5005, el);                      // long @5010, anchor 5010
    s.add(20, 'S', 5100, 30);
    s.exec(20, 10);                       // print 5100: anchor -> 5100
    CHECK_EQ(s.om->position(), 1);        // 5100 * 0.99 = 5049 > print? no exit
    s.add(21, 'B', 5040, 80);
    s.exec(21, 10);                       // print 5040 <= 5049: trail fires
    CHECK_EQ(s.om->position(), 0);
    CHECK_EQ(s.om->trades().size(), 1u);
    CHECK_EQ(s.om->trades()[0].reason, RSN_TRAIL);
}

static void test_take_profit() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    cfg.entry_cross = true;
    OmSim s = make_two_sided(cfg);
    Signals el = sig_el();
    el.tp = 0.01f;
    s.bar(5005, el);                      // long @5010; tp at 5060.1
    s.add(20, 'B', 5070, 80);
    s.exec(20, 10);                       // print 5070 >= tp: exit
    CHECK_EQ(s.om->position(), 0);
    CHECK_EQ(s.om->trades().size(), 1u);
    CHECK_EQ(s.om->trades()[0].reason, RSN_TP);
}

static void test_short_side_stop() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    cfg.entry_cross = true;
    OmSim s = make_two_sided(cfg);
    Signals es = sig_es();
    es.stop = 0.01f;
    s.bar(5005, es);                      // short 20 @ 5000-2=4998 crossing bid
    CHECK_EQ(s.om->position(), -1);
    s.add(20, 'S', 5055, 80);
    s.exec(20, 10);                       // print 5055 >= 4998*1.01=5047.98
    CHECK_EQ(s.om->position(), 0);
    CHECK_EQ(s.om->trades().size(), 1u);
    CHECK_EQ(s.om->trades()[0].reason, RSN_STOP);
    CHECK_EQ(s.om->trades()[0].side, -1);
}

static void test_join_entry_fill_then_signal_exit() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;   // 20 shares @ 5000
    cfg.fee = 0.0f;
    OmSim s = make_two_sided(cfg);
    s.bar(5005, sig_el());                // rest at 5000, qa=100
    s.add(20, 'B', 5000, 30);             // behind us
    s.exec(20, 25);                       // through -> fills us 20 (full)
    CHECK_EQ(s.om->position(), 1);
    s.bar(5005, sig_xl());
    CHECK_EQ(s.om->position(), 0);
    const TradeRecT& tr = s.om->trades()[0];
    CHECK_EQ((int)tr.entry_px, 5000);     // maker entry at the touch
    CHECK_EQ((int)tr.exit_px, 5000);      // crossed into the 5000 bid
    CHECK_EQ(tr.entry_t, 1);              // filled while bar 1 was forming
}

static void test_repeg_follows_the_touch() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    OmSim s = make_two_sided(cfg);
    s.bar(5005, sig_el());                // join 5000
    CHECK_EQ(s.working()->price, 5000u);
    s.add(20, 'B', 5002, 60);             // best bid moves to 5002
    s.bar(5005, sig_el());                // bar boundary: re-peg
    const OpenOrder* o = s.working();
    CHECK(o != nullptr);
    CHECK_EQ(o->price, 5002u);
    CHECK_EQ(o->queue_ahead, 60u);        // fresh queue behind the new level
    CHECK_EQ(s.om->position(), 0);
}

static void test_finish_force_closes() {
    OmConfig cfg;
    cfg.equity0 = 100000.0f;
    cfg.entry_cross = true;
    cfg.fee = 0.0f;
    OmSim s = make_two_sided(cfg);
    s.bar(5005, sig_el());                // long 19 @ 5010
    s.exec(10, 10);                       // a print at 5000 (the mark)
    s.om->finish(++s.ts);
    CHECK_EQ(s.om->position(), 0);
    CHECK_EQ(s.om->trades().size(), 1u);
    const TradeRecT& tr = s.om->trades()[0];
    CHECK_EQ(tr.reason, RSN_EOD);
    CHECK_EQ((int)tr.exit_px, 5000);      // marked out at the last print
    CHECK(std::fabs(s.om->cash() - (cfg.equity0 + tr.pnl)) < 0.01f);
}

static void test_no_entry_on_empty_book() {
    OmSim s;                              // no orders at all
    s.bar(5005, sig_el());
    CHECK(s.working() == nullptr);
    CHECK_EQ(s.om->position(), 0);
    s.om->finish(++s.ts);                 // nothing to close, no crash
    CHECK(s.om->trades().empty());
}

void run_order_manager_tests() {
    RUN_TEST(test_flat_entry_join_rests_at_touch);
    RUN_TEST(test_conflicting_entries_cancel_out);
    RUN_TEST(test_no_flip_while_positioned);
    RUN_TEST(test_cross_entry_fills_and_signal_exit);
    RUN_TEST(test_fee_arithmetic);
    RUN_TEST(test_stop_on_trade_print);
    RUN_TEST(test_trail_advances_with_prints);
    RUN_TEST(test_take_profit);
    RUN_TEST(test_short_side_stop);
    RUN_TEST(test_join_entry_fill_then_signal_exit);
    RUN_TEST(test_repeg_follows_the_touch);
    RUN_TEST(test_finish_force_closes);
    RUN_TEST(test_no_entry_on_empty_book);
}
