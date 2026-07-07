// SPSC queue, latency histogram, and endian helpers.
#include "test_harness.hpp"
#include "../util/spsc_queue.hpp"
#include "../util/latency_hist.hpp"
#include "../util/endian.hpp"

#include <thread>
#include <vector>
#include <cstdint>

using namespace ob;

static void test_endian_roundtrip() {
    uint8_t buf[8];
    store_be16(buf, 0x1234);          CHECK_EQ(be16(buf), 0x1234u);
    store_be32(buf, 0x89ABCDEFu);     CHECK_EQ(be32(buf), 0x89ABCDEFu);
    store_be64(buf, 0x0123456789ABCDEFull);
    CHECK_EQ(be64(buf), 0x0123456789ABCDEFull);
    store_be48(buf, 0xAABBCCDDEEFFull);
    CHECK_EQ(be48(buf), 0xAABBCCDDEEFFull);
    // big-endian byte order on the wire
    store_be16(buf, 0x00FF);
    CHECK_EQ(buf[0], 0x00); CHECK_EQ(buf[1], 0xFF);
}

static void test_spsc_single_thread() {
    SPSCQueue<int, 8> q;
    CHECK(q.empty());
    for (int i = 0; i < 7; ++i) CHECK(q.push(i));  // capacity N-? fills to N
    CHECK(q.push(7));                               // 8th ok (N=8)
    CHECK(!q.push(99));                             // full
    int v = -1;
    for (int i = 0; i < 8; ++i) { CHECK(q.pop(v)); CHECK_EQ(v, i); }
    CHECK(!q.pop(v));                               // empty
}

// Producer/consumer across two real threads; every value arrives in order.
static void test_spsc_threaded() {
    static SPSCQueue<uint64_t, 1024> q;
    const uint64_t N = 2'000'000;
    std::thread prod([&]{
        uint64_t i = 0;
        while (i < N) if (q.push(i)) ++i;
    });
    uint64_t expect = 0, got;
    int failures = 0;
    while (expect < N) {
        if (q.pop(got)) { if (got != expect) ++failures; ++expect; }
    }
    prod.join();
    CHECK_EQ(failures, 0);
    CHECK_EQ(expect, N);
}

static void test_latency_hist() {
    LatencyHist h(10000);
    for (int i = 0; i < 1000; ++i) h.record(double(i));   // 0..999 ns uniform
    CHECK_EQ(h.count(), 1000ull);
    // p50 ~ 500, p99 ~ 990
    CHECK(h.percentile(0.50) >= 490 && h.percentile(0.50) <= 510);
    CHECK(h.percentile(0.99) >= 980 && h.percentile(0.99) <= 1000);
    CHECK(h.max() >= 999);
    // overflow bucket
    h.record(50000);
    CHECK(h.max() >= 10000);
}

void run_util_tests() {
    RUN_TEST(test_endian_roundtrip);
    RUN_TEST(test_spsc_single_thread);
    RUN_TEST(test_spsc_threaded);
    RUN_TEST(test_latency_hist);
}

#ifdef TEST_STANDALONE
int main() { run_util_tests(); return obtest::summary(); }
#endif
