// VWAP / trade-count / imbalance correctness.
#include "test_harness.hpp"
#include "../parser/itch_parser.hpp"
#include "../book/order_book.hpp"
#include "../stats/stats_engine.hpp"
#include "../util/itch_gen.hpp"

#include <unordered_map>
#include <vector>

using namespace ob;

// Independently recompute VWAP while parsing, then compare to StatsEngine.
static void test_vwap_matches_independent() {
    GenConfig cfg; cfg.num_symbols = 2; cfg.seed = 321;
    ItchGenerator gen(cfg);
    std::vector<uint8_t> stream = gen.generate(40000);

    BookEngine fast(1'000'000, 1u << 18);
    StatsEngine stats;

    // independent oracle for trade prices
    struct O { uint32_t price; uint32_t shares; };
    std::unordered_map<uint64_t, O> orders;
    unsigned long long exp_num = 0, exp_den = 0, exp_trades = 0;

    ItchParser::parse_stream(stream.data(), stream.size(),
        [&](const DecodedMessage& m) {
            // oracle update (mirror book semantics for price resolution)
            switch (m.type) {
                case MsgType::AddOrder:
                case MsgType::AddOrderMPID:
                    orders[m.order_ref] = {m.price, m.shares}; break;
                case MsgType::OrderExecuted: {
                    auto it = orders.find(m.order_ref);
                    if (it != orders.end()) {
                        uint32_t q = m.shares > it->second.shares ? it->second.shares : m.shares;
                        exp_num += (unsigned long long)it->second.price * q;
                        exp_den += q; ++exp_trades;
                        it->second.shares -= q;
                        if (it->second.shares == 0) orders.erase(it);
                    }
                    break; }
                case MsgType::OrderExecutedPrice: {
                    auto it = orders.find(m.order_ref);
                    if (it != orders.end()) {
                        uint32_t q = m.shares > it->second.shares ? it->second.shares : m.shares;
                        if (m.printable) { exp_num += (unsigned long long)m.price * q;
                                           exp_den += q; ++exp_trades; }
                        it->second.shares -= q;
                        if (it->second.shares == 0) orders.erase(it);
                    }
                    break; }
                case MsgType::OrderCancel: {
                    auto it = orders.find(m.order_ref);
                    if (it != orders.end()) {
                        uint32_t q = m.shares > it->second.shares ? it->second.shares : m.shares;
                        it->second.shares -= q;
                        if (it->second.shares == 0) orders.erase(it);
                    }
                    break; }
                case MsgType::OrderDelete:
                    orders.erase(m.order_ref); break;
                case MsgType::OrderReplace:
                    orders.erase(m.order_ref);
                    orders[m.new_order_ref] = {m.price, m.shares}; break;
                case MsgType::Trade:
                    exp_num += (unsigned long long)m.price * m.shares;
                    exp_den += m.shares; ++exp_trades; break;
                default: break;
            }
            // engine path
            BookEngine::ExecResult ex;
            fast.apply(m, &ex);
            stats.on_message(m, ex);
        });

    CHECK_EQ(stats.vwap_numerator(),   exp_num);
    CHECK_EQ(stats.vwap_denominator(), exp_den);
    CHECK_EQ(stats.trade_count(),      exp_trades);
    CHECK(stats.vwap_units() > 0);
    // VWAP must lie within the generator's price band ($1..$100).
    CHECK(stats.vwap_units() >= 10000 && stats.vwap_units() <= 999990);
}

static void test_imbalance_and_spread() {
    BookEngine fast(1'000'000, 1u << 16);
    // Build a tiny known book on locate 1.
    auto add = [&](uint64_t ref, char side, uint32_t px, uint32_t sh) {
        DecodedMessage m; m.type = MsgType::AddOrder; m.stock_locate = 1;
        m.order_ref = ref; m.side = side; m.price = px; m.shares = sh;
        fast.apply(m);
    };
    add(1, SIDE_BUY, 100000, 500);   // bid $10.00 x500
    add(2, SIDE_BUY,  99900, 300);   // bid $9.99  x300
    add(3, SIDE_SELL,100100, 200);   // ask $10.01 x200
    add(4, SIDE_SELL,100200, 100);   // ask $10.02 x100
    const OrderBook* b = fast.book(1);
    CHECK(b != nullptr);
    CHECK_EQ(b->best_bid(), 100000u);
    CHECK_EQ(b->best_ask(), 100100u);
    CHECK_EQ(StatsEngine::spread(*b), 100u);          // 1 cent
    CHECK_EQ(StatsEngine::best_bid_depth(*b), 500u);
    CHECK_EQ(StatsEngine::best_ask_depth(*b), 200u);
    // imbalance = (800 - 300)/(800+300) = 0.4545...
    double imb = StatsEngine::imbalance(*b, 5);
    CHECK(imb > 0.45 && imb < 0.46);
}

void run_stats_tests() {
    RUN_TEST(test_vwap_matches_independent);
    RUN_TEST(test_imbalance_and_spread);
}

#ifdef TEST_STANDALONE
int main() { run_stats_tests(); return obtest::summary(); }
#endif
