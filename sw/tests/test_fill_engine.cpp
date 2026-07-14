// FillEngine queue-model tests: every update rule from fill_engine.hpp's
// header comment exercised with hand-scripted DecodedMessage sequences, plus
// the invariants (queue_ahead never grows while resting; maker fills only at
// our limit price; sum(fills) + remaining == original).
#include "test_harness.hpp"
#include "../book/order_book.hpp"
#include "../trade/fill_engine.hpp"

#include <vector>

using namespace ob;
using namespace ob::trade;

namespace {

constexpr uint16_t LOC = 1;

struct Sim {
    BookEngine eng{1 << 16, 1 << 12};  // small book: prices < 65536
    FillEngine fe{eng, LOC};
    uint64_t ts = 1000;
    std::vector<FillRec> fills;

    // Mimics trade_main ordering: FillEngine sees the message pre-apply.
    void step(DecodedMessage m) {
        m.timestamp = ++ts;
        fe.on_market_message(m);
        eng.apply(m);
        for (auto& f : fe.take_fills()) fills.push_back(f);
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
    void cancel(uint64_t ref, uint32_t sh) {
        DecodedMessage m;
        m.type = MsgType::OrderCancel;
        m.order_ref = ref; m.shares = sh; m.stock_locate = LOC;
        step(m);
    }
    void del(uint64_t ref) {
        DecodedMessage m;
        m.type = MsgType::OrderDelete;
        m.order_ref = ref; m.stock_locate = LOC;
        step(m);
    }
    void replace(uint64_t ref, uint64_t nref, uint32_t px, uint32_t sh) {
        DecodedMessage m;
        m.type = MsgType::OrderReplace;
        m.order_ref = ref; m.new_order_ref = nref; m.price = px; m.shares = sh;
        m.stock_locate = LOC;
        step(m);
    }
    void trade_msg(uint32_t px, uint32_t sh) {
        DecodedMessage m;
        m.type = MsgType::Trade;
        m.price = px; m.shares = sh; m.stock_locate = LOC; m.side = 'B';
        step(m);
    }

    uint64_t qa(uint64_t ref) {
        const OpenOrder* o = fe.find(ref);
        return o ? o->queue_ahead : (uint64_t)-1;
    }
    uint32_t rem(uint64_t ref) {
        const OpenOrder* o = fe.find(ref);
        return o ? o->remaining : 0;
    }
    uint32_t filled(uint64_t ref) {
        uint32_t s = 0;
        for (const auto& f : fills) if (f.our_ref == ref) s += f.shares;
        return s;
    }
};

}  // namespace

static void test_queue_snapshot_and_drain() {
    Sim s;
    s.add(10, 'B', 5000, 100);
    s.add(11, 'B', 5000, 50);
    uint64_t r = s.fe.submit_limit('B', 5000, 10, 1);
    CHECK_EQ(s.qa(r), 150u);              // everything displayed is ahead
    s.exec(10, 60);
    CHECK_EQ(s.qa(r), 90u);               // ahead-exec drains the queue
    s.exec(10, 40);
    CHECK_EQ(s.qa(r), 50u);
    s.cancel(11, 20);
    CHECK_EQ(s.qa(r), 30u);               // ahead-cancel drains it too
    s.del(11);
    CHECK_EQ(s.qa(r), 0u);                // full delete of the remainder
    CHECK_EQ(s.filled(r), 0u);            // exact accounting: no fills yet
    CHECK_EQ(s.rem(r), 10u);
}

static void test_traded_through_fills_us() {
    Sim s;
    s.add(10, 'B', 5000, 100);
    uint64_t r = s.fe.submit_limit('B', 5000, 10, 1);
    s.add(12, 'B', 5000, 30);             // arrives behind us
    s.exec(12, 30);                       // market trades through our position
    CHECK_EQ(s.filled(r), 10u);
    CHECK(s.fe.find(r) == nullptr);       // fully filled -> gone
    CHECK(!s.fills.empty() && s.fills.back().maker);
    CHECK_EQ(s.fills.back().price, 5000u);
    CHECK_EQ(s.qa(10), (uint64_t)-1);     // sanity: market ref is not ours
}

static void test_through_budget_shared_fifo() {
    Sim s;
    s.add(10, 'B', 5000, 50);
    uint64_t r1 = s.fe.submit_limit('B', 5000, 40, 1);
    uint64_t r2 = s.fe.submit_limit('B', 5000, 40, 2);
    CHECK_EQ(s.qa(r1), 50u);
    CHECK_EQ(s.qa(r2), 90u);              // includes our r1's 40
    s.add(13, 'B', 5000, 100);            // behind both
    s.exec(13, 60);                       // through: 60 shared FIFO
    CHECK_EQ(s.filled(r1), 40u);          // r1 takes 40
    CHECK_EQ(s.filled(r2), 20u);          // r2 gets the remaining 20, not 60
    CHECK_EQ(s.rem(r2), 20u);
    // r1's through-fill reduced r2's snapshot by 40 (it counted r1's shares);
    // r2's own fills never touch its own queue. Market 50 still ahead.
    CHECK_EQ(s.qa(r2), 50u);
    s.exec(10, 50);                       // ahead-exec drains the market 50
    CHECK_EQ(s.qa(r2), 0u);
    CHECK_EQ(s.rem(r2), 20u);             // queue clear, waiting for flow
    s.add(14, 'B', 5000, 20);
    s.exec(14, 20);                       // through again
    CHECK_EQ(s.filled(r2), 40u);          // completes
    CHECK(s.fe.open_orders().empty());
}

static void test_replace_into_our_level_behind_us() {
    Sim s;
    s.add(10, 'B', 5000, 100);
    uint64_t r = s.fe.submit_limit('B', 5000, 25, 1);
    s.exec(10, 100);                      // level drains exactly, no fill
    CHECK_EQ(s.qa(r), 0u);
    CHECK_EQ(s.filled(r), 0u);
    // An order from another price replaces INTO ours: the new ref is fresh
    // (monotonic), so it re-arrives behind us and fills us via traded-through.
    s.add(15, 'B', 4990, 80);
    s.replace(15, 16, 5000, 80);
    s.exec(16, 80);
    CHECK_EQ(s.filled(r), 25u);
    CHECK(s.fe.find(r) == nullptr);
}

static void test_leftover_defensive_maker_fill() {
    // The one queue-model branch invisible events can reach: an order that
    // predates our watermark appears at our level AFTER our snapshot (e.g. a
    // venue where replace keeps time priority). Its executions exceed our
    // believed queue_ahead and the excess fills us (maker, at our price).
    Sim s;
    s.add(10, 'B', 5000, 20);
    s.add(11, 'B', 4990, 50);
    uint64_t r = s.fe.submit_limit('B', 5000, 10, 1);   // qa=20, watermark=11
    CHECK_EQ(s.qa(r), 20u);
    s.replace(11, 5, 5000, 50);   // crafted: in-leg ref 5 <= watermark
    CHECK_EQ(s.qa(r), 20u);       // out-leg was another level; snapshot unchanged
    s.exec(5, 50);                // ahead path: take 20, leftover 30 -> fill 10
    CHECK_EQ(s.filled(r), 10u);
    CHECK(s.fe.find(r) == nullptr);
    CHECK(s.fills.back().maker);
    CHECK_EQ(s.fills.back().price, 5000u);
}

static void test_replace_out_leg_drains_queue() {
    Sim s;
    s.add(10, 'B', 5000, 70);
    uint64_t r = s.fe.submit_limit('B', 5000, 10, 1);
    CHECK_EQ(s.qa(r), 70u);
    s.replace(10, 11, 5010, 70);          // leaves our level entirely
    CHECK_EQ(s.qa(r), 0u);
    // the replacement rests at 5010, not our level: no fills, no queue effect
    CHECK_EQ(s.filled(r), 0u);
    CHECK_EQ(s.rem(r), 10u);
}

static void test_wrong_level_side_locate_ignored() {
    Sim s;
    s.add(10, 'B', 5000, 100);
    s.add(11, 'S', 5010, 100);
    uint64_t r = s.fe.submit_limit('B', 5000, 10, 1);
    CHECK_EQ(s.qa(r), 100u);
    s.exec(11, 50);                       // other side
    CHECK_EQ(s.qa(r), 100u);
    // A BETTER-priced bid executing is ahead of us on price: no queue effect
    // and no fill. (A worse-priced one would price-through fill us — see
    // test_price_through_fills_superior_order.)
    s.add(12, 'B', 5005, 100);
    s.exec(12, 100);
    CHECK_EQ(s.qa(r), 100u);
    // other locate: message resolved to locate 2, must be ignored
    DecodedMessage m;
    m.type = MsgType::AddOrder;
    m.order_ref = 13; m.side = 'B'; m.price = 5000; m.shares = 40;
    m.stock_locate = 2;
    s.step(m);
    DecodedMessage e;
    e.type = MsgType::OrderExecuted;
    e.order_ref = 13; e.shares = 40; e.stock_locate = 2;
    s.step(e);
    CHECK_EQ(s.qa(r), 100u);
    CHECK_EQ(s.filled(r), 0u);
}

static void test_trade_p_messages_ignored() {
    Sim s;
    s.add(10, 'B', 5000, 100);
    uint64_t r = s.fe.submit_limit('B', 5000, 10, 1);
    s.trade_msg(5000, 500);               // hidden liquidity print at our level
    CHECK_EQ(s.qa(r), 100u);              // no queue effect by design
    CHECK_EQ(s.filled(r), 0u);
}

static void test_our_cancel() {
    Sim s;
    s.add(10, 'B', 5000, 100);
    uint64_t r1 = s.fe.submit_limit('B', 5000, 10, 1);
    uint64_t r2 = s.fe.submit_limit('B', 5000, 10, 2);
    CHECK_EQ(s.qa(r2), 110u);
    CHECK(s.fe.cancel(r1));
    CHECK(!s.fe.cancel(r1));              // second cancel: not found
    CHECK_EQ(s.qa(r2), 100u);             // r1's 10 no longer ahead of r2
    CHECK(s.fe.find(r1) == nullptr);
    s.add(14, 'B', 5000, 10);
    s.exec(14, 10);                       // through -> r2 (r1 gone)
    CHECK_EQ(s.filled(r2), 10u);
    CHECK_EQ(s.filled(r1), 0u);
}

static void test_exec_clamped_to_resting_shares() {
    Sim s;
    s.add(10, 'B', 5000, 30);
    uint64_t r = s.fe.submit_limit('B', 5000, 10, 1);
    CHECK_EQ(s.qa(r), 30u);
    s.exec(10, 90);                       // over-sized exec: clamps to 30
    CHECK_EQ(s.qa(r), 0u);
    CHECK_EQ(s.filled(r), 0u);            // no phantom leftover fill
}

static void test_invariants_scripted() {
    Sim s;
    // Build a two-sided book and run a mixed script, checking invariants at
    // every step for one tracked order.
    s.add(10, 'B', 5000, 60);
    s.add(11, 'B', 4990, 80);
    s.add(12, 'S', 5010, 60);
    uint64_t r = s.fe.submit_limit('B', 5000, 50, 1);
    uint64_t prev_qa = s.qa(r);
    CHECK_EQ(prev_qa, 60u);

    struct Step { int kind; uint64_t ref; uint32_t sh; };
    const Step script[] = {
        {0, 20, 30},   // add B@5000 behind us
        {1, 10, 25},   // exec ahead
        {2, 10, 10},   // cancel ahead
        {1, 20, 15},   // exec behind (through -> fills us 15)
        {3, 10, 0},    // delete rest of 10
        {1, 20, 15},   // exec behind again (through -> fills us 15)
    };
    for (const Step& st : script) {
        if (st.kind == 0) s.add(st.ref, 'B', 5000, st.sh);
        else if (st.kind == 1) s.exec(st.ref, st.sh);
        else if (st.kind == 2) s.cancel(st.ref, st.sh);
        else s.del(st.ref);
        const OpenOrder* o = s.fe.find(r);
        if (o) {
            CHECK(o->queue_ahead <= prev_qa);        // never grows
            prev_qa = o->queue_ahead;
            CHECK_EQ(s.filled(r) + o->remaining, 50u);  // conservation
        }
    }
    CHECK_EQ(s.filled(r), 30u);
    CHECK_EQ(s.rem(r), 20u);
    for (const auto& f : s.fills) {
        CHECK(f.maker);
        CHECK_EQ(f.price, 5000u);                    // maker fills at our limit
        CHECK_EQ((int)f.side, (int)'B');
    }
}

static void test_marketable_sweep_multi_level() {
    Sim s;
    s.add(10, 'S', 5010, 30);
    s.add(11, 'S', 5011, 50);
    s.add(12, 'S', 5013, 100);            // 5012 empty: must be skipped
    uint64_t r = s.fe.submit_limit('B', 5013, 100, 7);
    auto fills = s.fe.take_fills();
    CHECK_EQ(fills.size(), 3u);
    CHECK_EQ(fills[0].price, 5010u); CHECK_EQ(fills[0].shares, 30u);
    CHECK_EQ(fills[1].price, 5011u); CHECK_EQ(fills[1].shares, 50u);
    CHECK_EQ(fills[2].price, 5013u); CHECK_EQ(fills[2].shares, 20u);
    for (const auto& f : fills) CHECK(!f.maker);
    CHECK(s.fe.find(r) == nullptr);       // nothing rested
}

static void test_sweep_partial_rest() {
    Sim s;
    s.add(10, 'S', 5010, 30);
    s.add(11, 'S', 5011, 50);
    uint64_t r = s.fe.submit_limit('B', 5011, 200, 7);
    auto fills = s.fe.take_fills();
    CHECK_EQ(fills.size(), 2u);           // 30 + 50 taker
    const OpenOrder* o = s.fe.find(r);
    CHECK(o != nullptr);
    CHECK_EQ(o->remaining, 120u);
    CHECK_EQ(o->price, 5011u);
    CHECK_EQ(o->queue_ahead, 0u);         // no displayed bids at 5011
}

static void test_sell_sweep_walks_down() {
    Sim s;
    s.add(10, 'B', 5008, 40);
    s.add(11, 'B', 5006, 40);
    uint64_t r = s.fe.submit_limit('S', 5006, 60, 7);
    auto fills = s.fe.take_fills();
    CHECK_EQ(fills.size(), 2u);
    CHECK_EQ(fills[0].price, 5008u); CHECK_EQ(fills[0].shares, 40u);
    CHECK_EQ(fills[1].price, 5006u); CHECK_EQ(fills[1].shares, 20u);
    CHECK(s.fe.find(r) == nullptr);
}

static void test_price_through_fills_superior_order() {
    // Our bid rests at a level with no market orders (crossed remainder).
    // Aggressive sells hitting bids BELOW our price would have hit us first.
    Sim s;
    s.add(10, 'B', 5000, 100);
    uint64_t r = s.fe.submit_limit('B', 5004, 30, 7);   // above best bid
    CHECK_EQ(s.qa(r), 0u);
    s.exec(10, 50);                       // sell flow hits the 5000 bid
    CHECK_EQ(s.filled(r), 30u);           // price-through: we fill at 5004
    CHECK(!s.fills.empty());
    CHECK(s.fills.back().maker);
    CHECK_EQ(s.fills.back().price, 5004u);
    CHECK(s.fe.find(r) == nullptr);
}

void run_fill_engine_tests() {
    RUN_TEST(test_marketable_sweep_multi_level);
    RUN_TEST(test_sweep_partial_rest);
    RUN_TEST(test_sell_sweep_walks_down);
    RUN_TEST(test_price_through_fills_superior_order);
    RUN_TEST(test_queue_snapshot_and_drain);
    RUN_TEST(test_traded_through_fills_us);
    RUN_TEST(test_through_budget_shared_fifo);
    RUN_TEST(test_replace_into_our_level_behind_us);
    RUN_TEST(test_leftover_defensive_maker_fill);
    RUN_TEST(test_replace_out_leg_drains_queue);
    RUN_TEST(test_wrong_level_side_locate_ignored);
    RUN_TEST(test_trade_p_messages_ignored);
    RUN_TEST(test_our_cancel);
    RUN_TEST(test_exec_clamped_to_resting_shares);
    RUN_TEST(test_invariants_scripted);
}
