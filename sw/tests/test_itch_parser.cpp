// ITCH decode correctness and SIMD header-scan checks.
#include "test_harness.hpp"
#include "../parser/itch_parser.hpp"
#include "../util/itch_gen.hpp"
#include "../util/endian.hpp"

#include <vector>
#include <cstring>

using namespace ob;

// Build one Add Order body by hand and confirm every field decodes correctly,
// exercising the big-endian byte-swaps.
static void test_decode_add_fields() {
    uint8_t body[blen::ADD] = {0};
    body[hdr::TYPE] = 'A';
    store_be16(body + hdr::STOCK_LOC, 7);
    store_be16(body + hdr::TRACK_NUM, 3);
    store_be48(body + hdr::TIMESTAMP + 2, 123456789ULL);
    store_be64(body + off::ADD_REF, 0x1122334455667788ULL);
    body[off::ADD_SIDE] = 'B';
    store_be32(body + off::ADD_SHARES, 250);
    std::memcpy(body + off::ADD_STOCK, "AAPL    ", 8);
    store_be32(body + off::ADD_PRICE, 1500600);   // $150.06

    DecodedMessage m = ItchParser::decode_body(body, blen::ADD);
    CHECK(m.type == MsgType::AddOrder);
    CHECK_EQ(m.stock_locate, 7);
    CHECK_EQ(m.timestamp, 123456789ULL);
    CHECK_EQ(m.order_ref, 0x1122334455667788ULL);
    CHECK_EQ(m.side, 'B');
    CHECK_EQ(m.shares, 250u);
    CHECK_EQ(m.price, 1500600u);
    CHECK(std::memcmp(m.stock, "AAPL    ", 8) == 0);
}

static void test_decode_replace_and_exec() {
    uint8_t u[blen::REPLACE] = {0};
    u[hdr::TYPE] = 'U';
    store_be64(u + off::REPL_ORIG_REF, 1000);
    store_be64(u + off::REPL_NEW_REF, 2000);
    store_be32(u + off::REPL_SHARES, 75);
    store_be32(u + off::REPL_PRICE, 990000);
    DecodedMessage m = ItchParser::decode_body(u, blen::REPLACE);
    CHECK(m.type == MsgType::OrderReplace);
    CHECK_EQ(m.order_ref, 1000ull);
    CHECK_EQ(m.new_order_ref, 2000ull);
    CHECK_EQ(m.shares, 75u);
    CHECK_EQ(m.price, 990000u);

    uint8_t e[blen::EXEC] = {0};
    e[hdr::TYPE] = 'E';
    store_be64(e + off::EXEC_REF, 555);
    store_be32(e + off::EXEC_SHARES, 40);
    store_be64(e + off::EXEC_MATCH, 99);
    DecodedMessage me = ItchParser::decode_body(e, blen::EXEC);
    CHECK(me.type == MsgType::OrderExecuted);
    CHECK_EQ(me.order_ref, 555ull);
    CHECK_EQ(me.shares, 40u);
    CHECK_EQ(me.match_number, 99ull);
}

// Parse a generated stream; per-type decoded counts must match generator stats.
static void test_stream_counts_match_generator() {
    GenConfig cfg; cfg.num_symbols = 3; cfg.seed = 999;
    ItchGenerator gen(cfg);
    std::vector<uint8_t> s = gen.generate(20000);
    const GenStats& gs = gen.stats();

    uint64_t per_type[IDX_COUNT] = {0};
    uint64_t total = 0;
    ParseStats ps = ItchParser::parse_stream(s.data(), s.size(),
        [&](const DecodedMessage& m) { ++per_type[msg_type_idx(m.type)]; ++total; });

    CHECK_EQ(ps.unknown, 0ull);
    CHECK_EQ(total, gs.total);
    for (int i = 0; i < IDX_COUNT; ++i) CHECK_EQ(per_type[i], gs.per_type[i]);
}

// SIMD batch type-byte counter must equal a scalar count.
static void test_simd_count_type() {
    GenConfig cfg; ItchGenerator gen(cfg);
    std::vector<uint8_t> s = gen.generate(5000);
    for (uint8_t tb : {uint8_t('A'), uint8_t('D'), uint8_t('U'), uint8_t(0x00)}) {
        size_t simd = ItchParser::simd_count_type(s.data(), s.size(), tb);
        size_t scalar = 0;
        for (uint8_t c : s) if (c == tb) ++scalar;
        CHECK_EQ(simd, scalar);
    }
}

static void test_udp_strip() {
    std::vector<uint8_t> frame(UDP_PAYLOAD_OFFSET + 10, 0xAB);
    for (size_t i = 0; i < UDP_PAYLOAD_OFFSET; ++i) frame[i] = 0x11;  // header
    const uint8_t* pl = nullptr; size_t pl_len = 0;
    CHECK(ItchParser::strip_udp(frame.data(), frame.size(), &pl, &pl_len));
    CHECK_EQ(pl_len, size_t(10));
    CHECK(pl[0] == 0xAB);
    // too-short frame rejected
    CHECK(!ItchParser::strip_udp(frame.data(), 10, &pl, &pl_len));
}

void run_itch_parser_tests() {
    RUN_TEST(test_decode_add_fields);
    RUN_TEST(test_decode_replace_and_exec);
    RUN_TEST(test_stream_counts_match_generator);
    RUN_TEST(test_simd_count_type);
    RUN_TEST(test_udp_strip);
}

#ifdef TEST_STANDALONE
int main() { run_itch_parser_tests(); return obtest::summary(); }
#endif
