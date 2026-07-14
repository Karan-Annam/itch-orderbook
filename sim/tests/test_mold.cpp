// MoldUDP64 decap + sequence tracking (mold_stripper, mold_mode=1).
//
// Session under test (expected sequence evolves 1000 -> 1008):
//   pkt  seq=1000 x2 msgs   applied            expected -> 1002
//   hb   seq=1002           no-op              expected -> 1002
//   pkt  seq=1002 x2 msgs   applied            expected -> 1004
//   pkt  seq=1006 x1 msg    GAP of 2, applied  expected -> 1007  (accept-from-here)
//   pkt  seq=1002 x2 msgs   STALE duplicate    dropped WHOLE, expected unchanged
//   pkt  seq=1007 x1 msg    applied            expected -> 1008
//   end  seq=1008           sticky session_end
// The six applied messages must produce a book identical to the reference
// model replaying them bare — proving decap forwards the framer's exact diet
// and that drops/heartbeats/gaps never disturb framing alignment.
#include "scenario.hpp"
#include "../mold_build.hpp"

using namespace obsim;

// Drive UDP frames (with in_sop per datagram) until all are sent and nmsgs
// commits observed; then a short drain so sticky telemetry settles.
static uint64_t drive_frames(RtlDriver& drv,
                             const std::vector<std::vector<uint8_t>>& frames,
                             uint64_t nmsgs) {
    auto* top = drv.top();
    size_t dg = 0, i = 0;
    uint64_t committed = 0, last = top->msg_total;
    while ((dg < frames.size() || committed < nmsgs) && drv.cycles() < 5'000'000ULL) {
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
    for (int t = 0; t < 20; ++t) { top->in_valid = 0; top->in_sop = 0; drv.tick(); }
    return committed;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "mold"};

    // messages (each already [len][body], i.e. a mold block)
    auto mA1 = add(1, 'B', 100, 100000, 1);
    auto mA2 = add(2, 'B', 200,  99900, 1);
    auto mB1 = add(3, 'S', 150, 100100, 1);
    auto mB2 = exec(1, 50);
    auto mC1 = del(3);
    auto mD1 = add(4, 'S',  60, 100150, 1);

    std::vector<std::vector<uint8_t>> frames = {
        udp_wrap(mold_packet(1000, {mA1, mA2})),
        udp_wrap(mold_heartbeat(1002)),
        udp_wrap(mold_packet(1002, {mB1, mB2})),
        udp_wrap(mold_packet(1006, {mC1})),       // gap: 1004,1005 lost
        udp_wrap(mold_packet(1002, {mB1, mB2})),  // stale duplicate: must drop
        udp_wrap(mold_packet(1007, {mD1})),
        udp_wrap(mold_session_end(1008)),
    };
    const uint64_t N = 6;  // messages that must apply (stale pkt contributes 0)

    // reference: the six applied messages replayed bare
    RefBook ref;
    {
        std::vector<uint8_t> s;
        for (auto* m : {&mA1, &mA2, &mB1, &mB2, &mC1, &mD1}) append(s, *m);
        auto msgs = split_messages(s, 0);
        for (auto& m : msgs) ref.apply(decode_body(m.body));
    }

    RtlDriver drv; drv.reset(false);        // raw_mode=0: UDP stripper active
    drv.top()->mold_mode = 1;
    uint64_t committed = drive_frames(drv, frames, N);
    auto* top = drv.top();

    SCHECK(ctx, committed == N, "committed %llu/%llu",
           (unsigned long long)committed, (unsigned long long)N);

    // book equals the bare replay (decap correctness + framing intact)
    SCHECK(ctx, top->best_bid_price == ref.best_bid_price() &&
                top->best_bid_shares == ref.best_bid_shares(),
           "best_bid rtl=%u(%u) ref=%u(%llu)", (unsigned)top->best_bid_price,
           (unsigned)top->best_bid_shares, ref.best_bid_price(),
           (unsigned long long)ref.best_bid_shares());
    SCHECK(ctx, top->best_ask_price == ref.best_ask_price() &&
                top->best_ask_shares == ref.best_ask_shares(),
           "best_ask rtl=%u(%u) ref=%u(%llu)", (unsigned)top->best_ask_price,
           (unsigned)top->best_ask_shares, ref.best_ask_price(),
           (unsigned long long)ref.best_ask_shares());
    SCHECK(ctx, top->tot_bid_vol == ref.tot_bid_vol() &&
                top->tot_ask_vol == ref.tot_ask_vol(),
           "vols rtl=%llu/%llu ref=%llu/%llu",
           (unsigned long long)top->tot_bid_vol, (unsigned long long)top->tot_ask_vol,
           (unsigned long long)ref.tot_bid_vol(), (unsigned long long)ref.tot_ask_vol());

    // sequence telemetry
    SCHECK(ctx, top->mold_gap_events == 1, "gap_events=%u exp=1",
           (unsigned)top->mold_gap_events);
    SCHECK(ctx, top->mold_gap_msgs == 2, "gap_msgs=%llu exp=2",
           (unsigned long long)top->mold_gap_msgs);
    SCHECK(ctx, top->mold_next_seq == 1008, "next_seq=%llu exp=1008",
           (unsigned long long)top->mold_next_seq);
    SCHECK(ctx, top->mold_session_end == 1, "session_end not sticky");

    // the stale duplicate must not have double-applied (exec(1,50) twice would
    // leave ref 1 at 0 shares; book totals above already prove it, but check
    // the message counters too: 4 adds, 1 exec, 1 delete — not 5/2/1)
    SCHECK(ctx, top->mc_add == 4 && top->mc_exec == 1 && top->mc_delete == 1,
           "counters add=%u exec=%u del=%u", (unsigned)top->mc_add,
           (unsigned)top->mc_exec, (unsigned)top->mc_delete);

    std::printf("[mold] %d checks, %d failures\n", ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
