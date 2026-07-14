// MoldUDP64 walker (sw/parser/mold_parser.hpp) — same sequence vectors as the
// RTL test (sim/tests/test_mold.cpp): gap accepted-from-here with lost-message
// accounting, stale packet skipped whole, heartbeat no-op, session-end stop,
// and decap(wrap(x)) == x round-trip through the generator.
#include "test_harness.hpp"
#include "../parser/mold_parser.hpp"
#include "../util/itch_gen.hpp"
#include "../util/endian.hpp"

#include <vector>

using namespace ob;

// local mold builders (mirror tools/mold_wrap.cpp encoding)
static void put_hdr(std::vector<uint8_t>& v, uint64_t seq, uint16_t count) {
    for (int i = 0; i < 10; ++i) v.push_back('S');
    for (int i = 7; i >= 0; --i) v.push_back(uint8_t(seq >> (8 * i)));
    v.push_back(uint8_t(count >> 8)); v.push_back(uint8_t(count & 0xFF));
}

// split a generated stream into per-message [len|body] blocks
static std::vector<std::vector<uint8_t>> split_blocks(const std::vector<uint8_t>& s) {
    std::vector<std::vector<uint8_t>> out;
    size_t o = 0;
    while (o + 2 <= s.size()) {
        uint16_t blen = uint16_t(s[o]) << 8 | s[o + 1];
        if (blen == 0 || o + 2 + blen > s.size()) break;
        out.emplace_back(s.begin() + long(o), s.begin() + long(o + 2 + blen));
        o += 2 + blen;
    }
    return out;
}

// wrap(x) then decap must reproduce x byte-for-byte (no gaps/stales involved)
static void test_roundtrip() {
    GenConfig cfg; cfg.num_symbols = 2;
    ItchGenerator gen(cfg);
    std::vector<uint8_t> itch = gen.generate(500);
    auto blocks = split_blocks(itch);

    std::vector<uint8_t> mold;
    uint64_t seq = 1;
    for (size_t m = 0; m < blocks.size(); m += 4) {
        size_t n = std::min<size_t>(4, blocks.size() - m);
        put_hdr(mold, seq, uint16_t(n));
        for (size_t i = 0; i < n; ++i)
            mold.insert(mold.end(), blocks[m + i].begin(), blocks[m + i].end());
        seq += n;
    }
    put_hdr(mold, seq, 0xFFFF);

    std::vector<uint8_t> plain;
    MoldStats st = MoldParser::decap(mold.data(), mold.size(), plain);
    CHECK(plain == itch);
    CHECK_EQ(st.messages, (uint64_t)blocks.size());
    CHECK_EQ(st.gap_events, 0ull);
    CHECK_EQ(st.stale_packets, 0ull);
    CHECK(st.session_end);
    CHECK(!st.truncated);
    CHECK_EQ(st.next_seq, seq);
}

// the RTL test's exact sequence scenario: 1000x2, hb, 1002x2, GAP 1006x1,
// stale 1002x2, 1007x1, end 1008
static void test_gap_stale_heartbeat_end() {
    GenConfig cfg; cfg.num_symbols = 1;
    ItchGenerator gen(cfg);
    auto blocks = split_blocks(gen.generate(8));
    CHECK(blocks.size() >= 6);

    std::vector<uint8_t> mold;
    auto pkt = [&](uint64_t seq, std::vector<int> idx) {
        put_hdr(mold, seq, uint16_t(idx.size()));
        for (int i : idx) mold.insert(mold.end(), blocks[size_t(i)].begin(),
                                      blocks[size_t(i)].end());
    };
    pkt(1000, {0, 1});
    put_hdr(mold, 1002, 0);          // heartbeat at expected seq
    pkt(1002, {2, 3});
    pkt(1006, {4});                  // gap: 1004,1005 lost
    pkt(1002, {2, 3});               // stale duplicate: skip whole
    pkt(1007, {5});
    put_hdr(mold, 1008, 0xFFFF);     // session end

    uint64_t applied = 0;
    MoldStats st = MoldParser::parse_stream(mold.data(), mold.size(),
                                            [&](const DecodedMessage&) { ++applied; });
    CHECK_EQ(applied, 6ull);
    CHECK_EQ(st.messages, 6ull);
    CHECK_EQ(st.gap_events, 1ull);
    CHECK_EQ(st.gap_msgs, 2ull);
    CHECK_EQ(st.stale_packets, 1ull);
    CHECK_EQ(st.heartbeats, 1ull);
    CHECK_EQ(st.next_seq, 1008ull);
    CHECK(st.session_end);
    // packets: 4 applied + 1 hb + 1 stale + 1 end
    CHECK_EQ(st.packets, 7ull);
}

// a truncated final packet must be reported, not walked off the end
static void test_truncated() {
    GenConfig cfg; cfg.num_symbols = 1;
    ItchGenerator gen(cfg);
    auto blocks = split_blocks(gen.generate(2));
    std::vector<uint8_t> mold;
    put_hdr(mold, 1, 2);
    mold.insert(mold.end(), blocks[0].begin(), blocks[0].end());
    mold.push_back(0x00);            // second block's length prefix cut short

    std::vector<uint8_t> plain;
    MoldStats st = MoldParser::decap(mold.data(), mold.size(), plain);
    CHECK(st.truncated);
    CHECK_EQ(st.messages, 1ull);
}

void run_mold_parser_tests() {
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_gap_stale_heartbeat_end);
    RUN_TEST(test_truncated);
}

#ifdef TEST_STANDALONE
int main() { run_mold_parser_tests(); return obtest::summary(); }
#endif
