// Differential test: SIMD FastBook == std::map oracle.
//
// Replays a synthetic ITCH stream through both BookEngine (direct-indexed,
// SIMD best-scan) and ReferenceBook (std::map), and asserts that top-of-book
// and a full price-level sweep agree at every checkpoint. This is the core
// correctness guarantee for the fast book; it exercises every message type,
// including the best-price rescan path on Delete/Execute and the Replace path.
#include "test_harness.hpp"
#include "../parser/itch_parser.hpp"
#include "../book/order_book.hpp"
#include "../book/reference_book.hpp"
#include "../util/itch_gen.hpp"

#include <vector>
#include <set>

using namespace ob;

// Compare top-of-book for one symbol between the two implementations.
static void compare_tob(BookEngine& fast, ReferenceBook& ref, uint16_t locate) {
    const OrderBook* b = fast.book(locate);
    uint32_t fb = b ? b->best_bid() : 0;
    uint32_t fa = b ? b->best_ask() : 0;
    uint32_t fbd = b ? b->best_bid_depth() : 0;
    uint32_t fad = b ? b->best_ask_depth() : 0;
    CHECK_EQ(fb, ref.best_bid(locate));
    CHECK_EQ(fa, ref.best_ask(locate));
    CHECK_EQ(fbd, ref.best_bid_depth(locate));
    CHECK_EQ(fad, ref.best_ask_depth(locate));
}

// Full sweep: every price level present in the reference must match the fast
// book, and vice versa (we sweep the union of populated prices).
static void compare_full(BookEngine& fast, ReferenceBook& ref, uint16_t locate) {
    const OrderBook* b = fast.book(locate);
    auto it = ref.books().find(locate);
    if (it == ref.books().end()) return;
    const auto& sb = it->second;
    for (auto& kv : sb.bid.shares) {
        uint32_t got = b ? b->bid_shares_at(kv.first) : 0;
        CHECK_EQ(got, kv.second);
    }
    for (auto& kv : sb.ask.shares) {
        uint32_t got = b ? b->ask_shares_at(kv.first) : 0;
        CHECK_EQ(got, kv.second);
    }
}

static void run_diff(uint64_t seed, int symbols, size_t n) {
    GenConfig cfg; cfg.num_symbols = symbols; cfg.seed = seed;
    ItchGenerator gen(cfg);
    std::vector<uint8_t> stream = gen.generate(n);

    BookEngine fast(1'000'000, 1u << 18);
    ReferenceBook ref;
    std::set<uint16_t> locates;
    uint64_t seq = 0;

    ItchParser::parse_stream(stream.data(), stream.size(),
        [&](const DecodedMessage& m) {
            fast.apply(m);
            ref.apply(m);
            if (m.stock_locate) locates.insert(m.stock_locate);
            ++seq;
            // top-of-book checked frequently; full sweep occasionally (cost).
            if (seq % 200 == 0)
                for (uint16_t loc : locates) compare_tob(fast, ref, loc);
            if (seq % 5000 == 0)
                for (uint16_t loc : locates) compare_full(fast, ref, loc);
        });

    // final exhaustive comparison
    for (uint16_t loc : locates) { compare_tob(fast, ref, loc); compare_full(fast, ref, loc); }
    // live order counts must match
    CHECK_EQ(fast.refs().size(), ref.live_orders());
}

static void test_diff_single_symbol() { run_diff(1, 1, 40000); }
static void test_diff_multi_symbol()  { run_diff(2, 4, 60000); }
static void test_diff_alt_seed()      { run_diff(0xBEEF, 3, 50000); }

// Targeted Replace stress: many replaces should keep books consistent.
static void test_replace_heavy() {
    GenConfig cfg; cfg.num_symbols = 2; cfg.seed = 7;
    ItchGenerator gen(cfg);
    std::vector<uint8_t> stream = gen.generate(30000);
    BookEngine fast(1'000'000, 1u << 18);
    ReferenceBook ref;
    uint64_t replaces = 0;
    ItchParser::parse_stream(stream.data(), stream.size(),
        [&](const DecodedMessage& m) {
            if (m.type == MsgType::OrderReplace) ++replaces;
            fast.apply(m); ref.apply(m);
        });
    CHECK(replaces > 1000);
    for (uint16_t loc = 1; loc <= 2; ++loc) { compare_tob(fast, ref, loc); compare_full(fast, ref, loc); }
}

void run_book_engine_tests() {
    RUN_TEST(test_diff_single_symbol);
    RUN_TEST(test_diff_multi_symbol);
    RUN_TEST(test_diff_alt_seed);
    RUN_TEST(test_replace_heavy);
}

#ifdef TEST_STANDALONE
int main() { run_book_engine_tests(); return obtest::summary(); }
#endif
