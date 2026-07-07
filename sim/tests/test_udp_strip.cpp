// test_udp_strip.cpp — udp_stripper header removal.
//
// Drives the SAME ITCH messages twice: once in raw mode (no wrapper) and once
// in UDP mode with a synthetic 42-byte Ethernet+IPv4+UDP header prepended and
// in_sop marking the first byte. Both must produce identical book state,
// proving the stripper discards exactly 42 bytes and forwards the payload.
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

// Drive a raw stream wrapped in a 42-byte UDP frame header, with in_sop on the
// first byte. Returns committed messages. The stripper is in UDP mode.
static uint64_t drive_udp(RtlDriver& drv, const std::vector<uint8_t>& itch,
                          uint64_t nmsgs, const std::function<void(uint64_t)>& cb) {
    auto* top = drv.top();
    // 42-byte header (arbitrary bytes) + ITCH payload, as one datagram.
    std::vector<uint8_t> frame(42, 0xEE);
    frame.insert(frame.end(), itch.begin(), itch.end());
    size_t i = 0; uint64_t committed = 0, last = top->msg_total;
    bool first = true;
    while (committed < nmsgs && drv.cycles() < 5'000'000ULL) {
        if (top->in_ready && i < frame.size()) {
            top->in_byte = frame[i]; top->in_valid = 1;
            top->in_sop = (first && i == 0) ? 1 : 0;
            ++i;
        } else { top->in_valid = 0; top->in_sop = 0; }
        drv.tick();
        if (i > 0) first = false;
        if (top->msg_total != last) { last = top->msg_total; cb(committed); ++committed; }
    }
    return committed;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "udp_strip"};
    std::vector<uint8_t> itch = book_stream();
    const uint64_t N = 7;

    // Raw-mode reference run.
    RefBook ref_raw; std::vector<RefMsg> rm;
    { auto msgs = split_messages(itch, 0); for (auto& m : msgs) rm.push_back(decode_body(m.body)); }
    RtlDriver raw; raw.reset(true);
    uint64_t c_raw = run_stream(raw, itch, N, [&](uint64_t){});
    SCHECK(ctx, c_raw == N, "raw committed %llu/%llu", (unsigned long long)c_raw, (unsigned long long)N);

    // UDP-mode run.
    RtlDriver udp; udp.reset(false);   // raw_mode=0 -> stripper active
    uint64_t c_udp = drive_udp(udp, itch, N, [&](uint64_t){});
    SCHECK(ctx, c_udp == N, "udp committed %llu/%llu", (unsigned long long)c_udp, (unsigned long long)N);

    // Both books must be identical, and match the reference.
    for (auto& m : rm) ref_raw.apply(m);
    auto* tr = raw.top(); auto* tu = udp.top();
    SCHECK(ctx, tr->best_bid_price == tu->best_bid_price, "best_bid raw=%u udp=%u",
           (unsigned)tr->best_bid_price, (unsigned)tu->best_bid_price);
    SCHECK(ctx, tr->best_ask_price == tu->best_ask_price, "best_ask raw=%u udp=%u",
           (unsigned)tr->best_ask_price, (unsigned)tu->best_ask_price);
    SCHECK(ctx, tu->best_bid_price == ref_raw.best_bid_price(),
           "udp best_bid=%u ref=%u", (unsigned)tu->best_bid_price, ref_raw.best_bid_price());
    SCHECK(ctx, tu->best_ask_price == ref_raw.best_ask_price(),
           "udp best_ask=%u ref=%u", (unsigned)tu->best_ask_price, ref_raw.best_ask_price());
    SCHECK(ctx, tu->tot_bid_vol == ref_raw.tot_bid_vol(), "udp tot_bid=%llu ref=%llu",
           (unsigned long long)tu->tot_bid_vol, (unsigned long long)ref_raw.tot_bid_vol());

    std::printf("[udp_strip] %d checks, %d failures\n", ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
