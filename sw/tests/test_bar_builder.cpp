// BarBuilder unit tests: bucket boundaries, gaps, late ticks, flush.
#include "test_harness.hpp"
#include "../trade/bar_builder.hpp"

using namespace ob::trade;

static constexpr uint64_t NS = 1'000'000'000;  // 1s bars in tests

static void test_first_trade_opens_bar() {
    BarBuilder bb(NS);
    CHECK_EQ(bb.on_trade(100, 5000, 10), 0);
    CHECK_EQ(bb.bars().size(), 0);
    CHECK(bb.has_open_bar());
}

static void test_same_bucket_aggregates() {
    BarBuilder bb(NS);
    bb.on_trade(100, 5000, 10);
    bb.on_trade(200, 5100, 5);   // new high
    bb.on_trade(300, 4900, 7);   // new low
    bb.on_trade(400, 5050, 3);   // close
    CHECK_EQ(bb.flush(), 1);
    const BarSeries& b = bb.bars();
    CHECK_EQ(b.size(), 1);
    CHECK_EQ((int)b.o[0], 5000);
    CHECK_EQ((int)b.h[0], 5100);
    CHECK_EQ((int)b.l[0], 4900);
    CHECK_EQ((int)b.c[0], 5050);
    CHECK_EQ((int)b.v[0], 25);
}

static void test_next_bucket_completes_bar() {
    BarBuilder bb(NS);
    bb.on_trade(100, 5000, 10);
    CHECK_EQ(bb.on_trade(NS + 100, 5200, 4), 1);  // second bucket
    const BarSeries& b = bb.bars();
    CHECK_EQ(b.size(), 1);
    CHECK_EQ((int)b.c[0], 5000);
    CHECK_EQ(bb.flush(), 1);
    CHECK_EQ((int)bb.bars().o[1], 5200);  // new bar opened at the new trade
    CHECK_EQ((int)bb.bars().v[1], 4);
}

static void test_bucket_boundary_exact() {
    // ts == k * bar_ns belongs to bucket k, not k-1
    BarBuilder bb(NS);
    bb.on_trade(NS - 1, 5000, 1);       // last ns of bucket 0
    CHECK_EQ(bb.on_trade(NS, 5100, 1), 1);  // first ns of bucket 1 closes it
    CHECK_EQ((int)bb.bars().c[0], 5000);
}

static void test_gap_emits_flat_bars() {
    BarBuilder bb(NS);
    bb.on_trade(100, 5000, 10);
    // jump 4 buckets: closes bar 0, emits flat bars for buckets 1..3
    CHECK_EQ(bb.on_trade(4 * NS + 5, 5300, 2), 4);
    const BarSeries& b = bb.bars();
    CHECK_EQ(b.size(), 4);
    for (int i = 1; i < 4; ++i) {  // flat: o=h=l=c=prev close, v=0
        CHECK_EQ((int)b.o[i], 5000);
        CHECK_EQ((int)b.h[i], 5000);
        CHECK_EQ((int)b.l[i], 5000);
        CHECK_EQ((int)b.c[i], 5000);
        CHECK_EQ((int)b.v[i], 0);
    }
    CHECK_EQ(bb.flush(), 1);
    CHECK_EQ((int)bb.bars().o[4], 5300);
}

static void test_late_tick_folds_into_open_bar() {
    BarBuilder bb(NS);
    bb.on_trade(NS + 100, 5000, 10);       // bucket 1
    CHECK_EQ(bb.on_trade(500, 5500, 2), 0);  // bucket 0: late, folds in
    CHECK_EQ(bb.flush(), 1);
    const BarSeries& b = bb.bars();
    CHECK_EQ(b.size(), 1);
    CHECK_EQ((int)b.h[0], 5500);
    CHECK_EQ((int)b.c[0], 5500);
    CHECK_EQ((int)b.v[0], 12);
}

static void test_flush_empty_and_idempotent() {
    BarBuilder bb(NS);
    CHECK_EQ(bb.flush(), 0);   // nothing open
    bb.on_trade(100, 5000, 1);
    CHECK_EQ(bb.flush(), 1);
    CHECK_EQ(bb.flush(), 0);   // already closed
    CHECK_EQ(bb.bars().size(), 1);
}

void run_bar_builder_tests() {
    RUN_TEST(test_first_trade_opens_bar);
    RUN_TEST(test_same_bucket_aggregates);
    RUN_TEST(test_next_bucket_completes_bar);
    RUN_TEST(test_bucket_boundary_exact);
    RUN_TEST(test_gap_emits_flat_bars);
    RUN_TEST(test_late_tick_folds_into_open_bar);
    RUN_TEST(test_flush_empty_and_idempotent);
}
