// test_udp_strip.cpp — udp_stripper header removal on the 16-byte word bus.
//
// Drives the SAME ITCH messages three ways: raw mode (no wrapper), UDP mode as
// one datagram (42-byte Ethernet+IPv4+UDP header, in_sop on the first word),
// and UDP mode split across TWO datagrams with the cut mid-message. All must
// produce identical book state, proving the stripper discards exactly 42
// bytes per datagram (whole words 0-1 plus 10 bytes of word 2), repacks the
// split word to lane 0, and resets its word counter on in_sop — including the
// case where the second datagram interrupts a partial message.
#include "scenario.hpp"

using namespace obsim;

// Build a small deterministic book-building stream.
static std::vector<uint8_t> book_stream() {
    std::vector<uint8_t> s;
    append(s, add(1, 'B', 100, 100000, 1));
    append(s, add(2, 'B', 200,  99900, 1));
    append(s, add(3, 'S', 150, 100100, 1));
    append(s, add(4, 'S',  50, 100200, 1));
    append(s, cancel(1, 40));
    append(s, del(3));
    append(s, replace(2, 5, 300, 99800));
    return s;
}

// Drive one or more datagrams in UDP mode. Each datagram = 42 header bytes +
// a payload slice; words are fed 16 bytes at a time with in_sop on the first
// word of each datagram and a partial word at each datagram tail. Returns
// committed messages.
static uint64_t drive_udp(RtlDriver& drv,
                          const std::vector<std::vector<uint8_t>>& payloads,
                          uint64_t nmsgs) {
    auto* top = drv.top();
    // Flatten to (byte, sop-word-flag) datagram frames.
    std::vector<std::vector<uint8_t>> frames;
    for (const auto& p : payloads) {
        std::vector<uint8_t> f(42, 0xEE);
        f.insert(f.end(), p.begin(), p.end());
        frames.push_back(std::move(f));
    }
    size_t dg = 0, i = 0;
    uint64_t committed = 0, last = top->msg_total;
    while (committed < nmsgs && drv.cycles() < 5'000'000ULL) {
        if (dg < frames.size() && top->in_ready) {
            const auto& f = frames[dg];
            size_t n = std::min(WORD_BYTES, f.size() - i);
            set_in_word(top, &f[i], n);
            top->in_valid = 1;
            top->in_sop   = (i == 0) ? 1 : 0;
            i += n;
            if (i >= f.size()) { ++dg; i = 0; }
        } else {
            top->in_valid = 0; top->in_sop = 0;
        }
        drv.tick();
        if (top->msg_total != last) { last = top->msg_total; ++committed; }
    }
    return committed;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "udp_strip"};
    std::vector<uint8_t> itch = book_stream();
    const uint64_t N = 7;

    // Reference book.
    RefBook ref;
    { auto msgs = split_messages(itch, 0);
      for (auto& m : msgs) ref.apply(decode_body(m.body)); }

    // Raw-mode run.
    RtlDriver raw; raw.reset(true);
    uint64_t c_raw = run_stream(raw, itch, N, [&](uint64_t){});
    SCHECK(ctx, c_raw == N, "raw committed %llu/%llu", (unsigned long long)c_raw, (unsigned long long)N);

    // UDP-mode run, single datagram.
    RtlDriver udp; udp.reset(false);   // raw_mode=0 -> stripper active
    uint64_t c_udp = drive_udp(udp, {itch}, N);
    SCHECK(ctx, c_udp == N, "udp committed %llu/%llu", (unsigned long long)c_udp, (unsigned long long)N);

    // UDP-mode run, two datagrams cut mid-message (and off any word boundary):
    // the ITCH byte stream must simply concatenate across datagrams.
    size_t cut = itch.size() / 2 + 3;
    std::vector<uint8_t> p1(itch.begin(), itch.begin() + cut);
    std::vector<uint8_t> p2(itch.begin() + cut, itch.end());
    RtlDriver udp2; udp2.reset(false);
    uint64_t c_udp2 = drive_udp(udp2, {p1, p2}, N);
    SCHECK(ctx, c_udp2 == N, "udp2 committed %llu/%llu", (unsigned long long)c_udp2, (unsigned long long)N);

    // All books must be identical, and match the reference.
    auto* tr = raw.top();
    for (auto* tu : {udp.top(), udp2.top()}) {
        SCHECK(ctx, tr->best_bid_price == tu->best_bid_price, "best_bid raw=%u udp=%u",
               (unsigned)tr->best_bid_price, (unsigned)tu->best_bid_price);
        SCHECK(ctx, tr->best_ask_price == tu->best_ask_price, "best_ask raw=%u udp=%u",
               (unsigned)tr->best_ask_price, (unsigned)tu->best_ask_price);
        SCHECK(ctx, tu->best_bid_price == ref.best_bid_price(),
               "udp best_bid=%u ref=%u", (unsigned)tu->best_bid_price, ref.best_bid_price());
        SCHECK(ctx, tu->best_ask_price == ref.best_ask_price(),
               "udp best_ask=%u ref=%u", (unsigned)tu->best_ask_price, ref.best_ask_price());
        SCHECK(ctx, tu->tot_bid_vol == ref.tot_bid_vol(), "udp tot_bid=%llu ref=%llu",
               (unsigned long long)tu->tot_bid_vol, (unsigned long long)ref.tot_bid_vol());
    }

    std::printf("[udp_strip] %d checks, %d failures\n", ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
