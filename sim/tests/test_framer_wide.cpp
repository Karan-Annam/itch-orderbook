// test_framer_wide.cpp — the 16-byte-per-beat framer's alignment machinery.
//
// Messages are packed back-to-back with no alignment, so the framer's
// compacting window must handle a message starting/ending at every byte
// offset within a word, a length prefix split across two words, and residual
// bytes held while the engine stalls input. Four scenarios:
//
//  1. Phase sweep — a Delete frame is 23 bytes and gcd(23,16)=1, so k leading
//     Deletes shift the following stream's alignment by 7k mod 16; k=0..15
//     covers all 16 phases (including the split length prefix). Each run
//     drives a mixed-type stream and diffs the final book vs the reference.
//  2. Beat-size equivalence — the same stream fed as full 16-byte words,
//     1-byte beats, and random-size beats must produce identical books
//     (partial beats are legal anywhere, per the packed-contiguous contract).
//  3. Back-to-back minimum-size frames — System messages are exactly one word
//     (2+14 bytes); 64 in a row, aligned and misaligned, must all commit.
//  4. Stall with residual bytes — long engine occupancy (best-price scans)
//     under full-rate input must drop in_ready with bytes parked in the
//     window, and lose nothing.
//  5. Malformed lengths — zero, short, and oversized frames are discarded
//     without committing stale decoder state or losing the following frame.
#include "scenario.hpp"

using namespace obsim;

// A mixed stream exercising every message type, with self-consistent refs.
static std::vector<uint8_t> mixed_stream() {
    std::vector<uint8_t> s;
    append(s, add(10, 'B', 100, 50000, 1));
    append(s, add(11, 'S',  80, 50100, 1));
    append(s, exec(10, 20));
    append(s, exec_price(11, 10, 50100));
    append(s, cancel(10, 30));
    append(s, replace(11, 12, 60, 50200));
    append(s, trade('B', 25, 50050));
    append(s, sysevt('Q'));
    append(s, del(12));
    append(s, add(13, 'S', 40, 50300, 1));
    return s;
}

// Final-book diff of an RTL run against the reference model.
static void check_book(TestCtx& ctx, const char* tag, Vorderbook_top* t, const RefBook& ref) {
    SCHECK(ctx, (bool)t->best_bid_valid == ref.best_bid_valid(), "%s bid_valid", tag);
    SCHECK(ctx, (bool)t->best_ask_valid == ref.best_ask_valid(), "%s ask_valid", tag);
    if (ref.best_bid_valid())
        SCHECK(ctx, t->best_bid_price == ref.best_bid_price(), "%s best_bid rtl=%u ref=%u",
               tag, (unsigned)t->best_bid_price, ref.best_bid_price());
    if (ref.best_ask_valid())
        SCHECK(ctx, t->best_ask_price == ref.best_ask_price(), "%s best_ask rtl=%u ref=%u",
               tag, (unsigned)t->best_ask_price, ref.best_ask_price());
    SCHECK(ctx, t->tot_bid_vol == ref.tot_bid_vol(), "%s tot_bid rtl=%llu ref=%llu",
           tag, (unsigned long long)t->tot_bid_vol, (unsigned long long)ref.tot_bid_vol());
    SCHECK(ctx, t->tot_ask_vol == ref.tot_ask_vol(), "%s tot_ask rtl=%llu ref=%llu",
           tag, (unsigned long long)t->tot_ask_vol, (unsigned long long)ref.tot_ask_vol());
}

static RefBook run_ref(const std::vector<uint8_t>& stream) {
    RefBook ref;
    for (auto& m : split_messages(stream, 0)) ref.apply(decode_body(m.body));
    return ref;
}

// Drive a stream with caller-chosen beat sizes; returns committed count and
// (optionally) how many cycles in_ready was low while input was pending.
static uint64_t drive_beats(RtlDriver& drv, const std::vector<uint8_t>& s,
                            uint64_t nmsgs,
                            const std::function<size_t(size_t /*left*/)>& pick_n,
                            uint64_t* stall_cycles = nullptr) {
    auto* top = drv.top();
    size_t i = 0; uint64_t committed = 0, last = top->msg_total;
    while (committed < nmsgs && drv.cycles() < 5'000'000ULL) {
        if (i < s.size() && !top->in_ready && stall_cycles) ++*stall_cycles;
        if (top->in_ready && i < s.size()) {
            size_t n = std::min(pick_n(s.size() - i), s.size() - i);
            set_in_word(top, &s[i], n);
            top->in_valid = 1;
            i += n;
        } else {
            top->in_valid = 0;
        }
        drv.tick();
        if (top->msg_total != last) { last = top->msg_total; ++committed; }
    }
    return committed;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "framer_wide"};

    // ---- 1. alignment phase sweep ------------------------------------------
    for (int k = 0; k < 16; ++k) {
        std::vector<uint8_t> s;
        for (int j = 0; j < k; ++j) append(s, del(900000 + j));  // unknown refs: no-op commits
        append(s, mixed_stream());
        const uint64_t n = uint64_t(k) + 10;
        RefBook ref = run_ref(s);

        RtlDriver drv; drv.reset(true);
        uint64_t c = run_stream(drv, s, n, [](uint64_t){});
        SCHECK(ctx, c == n, "phase %d committed %llu/%llu", k,
               (unsigned long long)c, (unsigned long long)n);
        char tag[32]; std::snprintf(tag, sizeof tag, "phase%d", k);
        check_book(ctx, tag, drv.top(), ref);
    }

    // ---- 2. beat-size equivalence ------------------------------------------
    {
        std::vector<uint8_t> s;
        for (int j = 0; j < 3; ++j) append(s, del(910000 + j));  // word-misaligned start
        append(s, mixed_stream());
        const uint64_t n = 13;
        RefBook ref = run_ref(s);

        RtlDriver full;  full.reset(true);
        RtlDriver bytes; bytes.reset(true);
        RtlDriver rnd;   rnd.reset(true);
        uint32_t lcg = 12345;
        uint64_t c1 = drive_beats(full,  s, n, [](size_t)   { return WORD_BYTES; });
        uint64_t c2 = drive_beats(bytes, s, n, [](size_t)   { return size_t(1); });
        uint64_t c3 = drive_beats(rnd,   s, n, [&](size_t)  {
            lcg = lcg * 1664525u + 1013904223u;
            return size_t(lcg % WORD_BYTES) + 1;
        });
        SCHECK(ctx, c1 == n && c2 == n && c3 == n, "beat-size commits %llu/%llu/%llu",
               (unsigned long long)c1, (unsigned long long)c2, (unsigned long long)c3);
        check_book(ctx, "beat16", full.top(),  ref);
        check_book(ctx, "beat1",  bytes.top(), ref);
        check_book(ctx, "beatR",  rnd.top(),   ref);
    }

    // ---- 3. back-to-back minimum-size frames --------------------------------
    for (int lead = 0; lead <= 1; ++lead) {
        std::vector<uint8_t> s;
        if (lead) append(s, del(920000));   // shifts every System frame off word alignment
        for (int j = 0; j < 64; ++j) append(s, sysevt('Q'));
        const uint64_t n = uint64_t(lead) + 64;

        RtlDriver drv; drv.reset(true);
        uint64_t c = run_stream(drv, s, n, [](uint64_t){});
        SCHECK(ctx, c == n, "min-size lead=%d committed %llu/%llu", lead,
               (unsigned long long)c, (unsigned long long)n);
        SCHECK(ctx, drv.top()->mc_system == 64, "min-size lead=%d mc_system=%u", lead,
               (unsigned)drv.top()->mc_system);
    }

    // ---- 4. stall under backpressure with residual bytes held ---------------
    {
        const int N = 20;
        std::vector<uint8_t> s;
        for (int j = 0; j < N; ++j)
            append(s, add(1000 + j, 'B', 10, uint32_t(60000 + j), 1));
        for (int j = N - 1; j >= 0; --j)
            append(s, del(1000 + j));       // always deletes the best bid -> scan
        const uint64_t n = 2 * N;
        RefBook ref = run_ref(s);

        RtlDriver drv; drv.reset(true);
        uint64_t stalls = 0;
        uint64_t c = drive_beats(drv, s, n, [](size_t) { return WORD_BYTES; }, &stalls);
        SCHECK(ctx, c == n, "stall committed %llu/%llu",
               (unsigned long long)c, (unsigned long long)n);
        SCHECK(ctx, stalls > 0, "in_ready never dropped under full-rate input");
        check_book(ctx, "stall", drv.top(), ref);
    }

    // ---- 5. malformed lengths are dropped and alignment recovers ------------
    {
        std::vector<uint8_t> s;
        s.push_back(0); s.push_back(0);                 // zero-length frame
        s.push_back(0); s.push_back(13);                // short Add body
        s.push_back('A');
        for (int i = 1; i < 13; ++i) s.push_back(0);
        s.push_back(0); s.push_back(49);                // decoder-overflow frame
        s.push_back('P');
        for (int i = 1; i < 49; ++i) s.push_back(uint8_t(i));
        append(s, sysevt('Q'));                         // must still commit

        RtlDriver drv; drv.reset(true);
        uint64_t c = drive_beats(drv, s, 1, [](size_t) { return WORD_BYTES; });
        SCHECK(ctx, c == 1, "malformed stream committed %llu valid messages",
               (unsigned long long)c);
        SCHECK(ctx, drv.top()->msg_total == 1, "malformed frames reached engine");
        SCHECK(ctx, drv.top()->mc_system == 1, "following System frame was lost");
    }

    std::printf("[framer_wide] %d checks, %d failures\n", ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
