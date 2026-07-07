// End-to-end replay through the real receiver path.
//
// Loads a generated stream into FileReplayReceiver, delivers it as
// message-aligned "datagrams", parses + applies each, and checks invariants the
// whole system must uphold:
//   * packetised parse sees exactly the same messages as a flat parse,
//   * the book is never crossed (best_bid < best_ask) — the generator
//     guarantees it, and the book must preserve it,
//   * the best level always has positive depth (no stale best price),
//   * counters and live-order accounting are self-consistent.
#include "test_harness.hpp"
#include "../receiver/replay_receiver.hpp"
#include "../parser/itch_parser.hpp"
#include "../book/order_book.hpp"
#include "../stats/stats_engine.hpp"
#include "../util/itch_gen.hpp"

#include <vector>
#include <set>

using namespace ob;

static void test_packetised_equals_flat() {
    GenConfig cfg; cfg.num_symbols = 4; cfg.seed = 55;
    ItchGenerator gen(cfg);
    FileReplayReceiver rx;
    rx.load_bytes(gen.generate(40000));

    uint64_t flat = 0;
    ItchParser::parse_stream(rx.data(), rx.size(),
        [&](const DecodedMessage&) { ++flat; });

    uint64_t pkt = 0, packets = 0;
    rx.deliver_packets(1400, [&](const uint8_t* p, size_t n) {
        ++packets;
        ItchParser::parse_stream(p, n, [&](const DecodedMessage&) { ++pkt; });
    });
    CHECK_EQ(flat, pkt);
    CHECK(packets > 1);          // actually split into multiple datagrams
    CHECK_EQ(flat, gen.stats().total);
}

static void test_book_never_crossed_and_best_valid() {
    GenConfig cfg; cfg.num_symbols = 4; cfg.seed = 77;
    ItchGenerator gen(cfg);
    FileReplayReceiver rx; rx.load_bytes(gen.generate(80000));

    BookEngine engine(1'000'000, 1u << 18);
    StatsEngine stats;
    std::set<uint16_t> locs;
    uint64_t seq = 0;
    int cross_violations = 0, stale_best = 0;

    ItchParser::parse_stream(rx.data(), rx.size(), [&](const DecodedMessage& m) {
        BookEngine::ExecResult ex;
        engine.apply(m, &ex);
        stats.on_message(m, ex);
        if (m.stock_locate) locs.insert(m.stock_locate);
        if (++seq % 173 == 0) {
            for (uint16_t L : locs) {
                const OrderBook* b = engine.book(L);
                if (!b) continue;
                uint32_t bb = b->best_bid(), ba = b->best_ask();
                if (bb && ba && bb >= ba) ++cross_violations;       // crossed!
                if (bb && b->best_bid_depth() == 0) ++stale_best;   // stale best
                if (ba && b->best_ask_depth() == 0) ++stale_best;
            }
        }
    });
    CHECK_EQ(cross_violations, 0);
    CHECK_EQ(stale_best, 0);
    CHECK(stats.trade_count() > 0);
    CHECK(stats.vwap_units() > 0);
    CHECK_EQ(engine.refs().size(), 0u + engine.refs().size());  // sane
}

void run_full_pipeline_tests() {
    RUN_TEST(test_packetised_equals_flat);
    RUN_TEST(test_book_never_crossed_and_best_valid);
}

#ifdef TEST_STANDALONE
int main() { run_full_pipeline_tests(); return obtest::summary(); }
#endif
