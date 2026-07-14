// Book benchmark: std::map reference vs direct-indexed + SIMD.
//
// Replays the same ITCH stream through both book implementations and reports
// messages/second and average per-message latency, plus the speedup. This
// quantifies the win from replacing tree traversal (O(log N) per level op,
// pointer-chasing, cache-unfriendly) with direct price indexing (O(1),
// sequential, SIMD best-price scan).
#include "../sw/parser/itch_parser.hpp"
#include "../sw/book/order_book.hpp"
#include "../sw/book/reference_book.hpp"
#include "../sw/util/itch_gen.hpp"
#include "../sw/util/rdtsc.hpp"
#include "../sw/util/cpu_affinity.hpp"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <string>

using namespace ob;

template <typename Fn>
static double time_ms(Fn&& fn) {
    const TscSample t0 = tsc_sample();
    fn();
    const TscSample t1 = tsc_sample();
    static TscClock clk = [] { TscClock c; c.calibrate(80); return c; }();
    static const uint64_t overhead = tsc_sample_overhead();
    if (t0.aux != t1.aux || t1.ticks < t0.ticks)
        std::fprintf(stderr, "[bench] warning: timed region migrated cores\n");
    const uint64_t elapsed = t1.ticks >= t0.ticks ? t1.ticks - t0.ticks : 0;
    return clk.to_ns(elapsed > overhead ? elapsed - overhead : 0) / 1e6;
}

int main(int argc, char** argv) {
    size_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 500000;
    const int core = (argc > 2) ? std::atoi(argv[2]) : 0;
    const bool pinned = pin_to_core(core);
    GenConfig cfg; cfg.num_symbols = 4;
    ItchGenerator gen(cfg);
    std::vector<uint8_t> stream = gen.generate(n);

    // Pre-decode once so we time only the book, not the parser.
    std::vector<DecodedMessage> msgs;
    msgs.reserve(gen.stats().total);
    ItchParser::parse_stream(stream.data(), stream.size(),
        [&](const DecodedMessage& m) { msgs.push_back(m); });

    std::printf("Benchmark: %zu messages, 4 symbols, SIMD tier: %s\n\n",
                msgs.size(), simd_tier());
    std::printf("affinity: core %d (%s)\n\n", core, pinned ? "pinned" : "unsupported");

    // Warm the instruction path, then alternate execution order across seven
    // independent trials. Reporting the median avoids making a speedup claim
    // from a single scheduler/cache-favoured run.
    const size_t warm_n = std::min<size_t>(msgs.size(), 20'000);
    {
        ReferenceBook warm_ref;
        BookEngine warm_fast(1'000'000, 1u << 21);
        for (size_t i = 0; i < warm_n; ++i) {
            warm_ref.apply(msgs[i]);
            warm_fast.apply(msgs[i]);
        }
    }

    constexpr int trials = 7;
    std::vector<double> ref_ms, fast_ms;
    ref_ms.reserve(trials); fast_ms.reserve(trials);
    auto run_ref = [&] {
        ReferenceBook book;
        return time_ms([&] { for (const auto& m : msgs) book.apply(m); });
    };
    auto run_fast = [&] {
        BookEngine book(1'000'000, 1u << 21);
        return time_ms([&] { for (const auto& m : msgs) book.apply(m); });
    };
    for (int trial = 0; trial < trials; ++trial) {
        if ((trial & 1) == 0) {
            ref_ms.push_back(run_ref()); fast_ms.push_back(run_fast());
        } else {
            fast_ms.push_back(run_fast()); ref_ms.push_back(run_ref());
        }
    }
    std::sort(ref_ms.begin(), ref_ms.end());
    std::sort(fast_ms.begin(), fast_ms.end());
    const double ms_ref = ref_ms[trials / 2];
    const double ms_fast = fast_ms[trials / 2];

    auto mps = [&](double ms) { return msgs.size() / (ms / 1000.0) / 1e6; };
    auto nsop = [&](double ms) { return ms * 1e6 / msgs.size(); };

    std::printf("trials: %d (median; process pinned when supported)\n\n", trials);
    std::printf("%-28s %10s %14s %12s\n", "implementation", "time(ms)", "Mmsg/s", "ns/msg");
    std::printf("%-28s %10.1f %14.2f %12.1f\n",
                "std::map (RB-tree)", ms_ref, mps(ms_ref), nsop(ms_ref));
    std::printf("%-28s %10.1f %14.2f %12.1f\n",
                "direct-index + SIMD", ms_fast, mps(ms_fast), nsop(ms_fast));
    std::printf("\nSpeedup: %.2fx\n", ms_ref / ms_fast);
    std::printf("trial ranges: std::map %.1f..%.1f ms; SIMD %.1f..%.1f ms\n",
                ref_ms.front(), ref_ms.back(), fast_ms.front(), fast_ms.back());

    ReferenceBook ref;
    BookEngine fast(1'000'000, 1u << 21);
    for (const auto& m : msgs) { ref.apply(m); fast.apply(m); }
    bool match = true;
    for (uint16_t locate = 1; locate <= cfg.num_symbols; ++locate) {
        const OrderBook* book = fast.book(locate);
        match &= book && book->best_bid() == ref.best_bid(locate)
                      && book->best_ask() == ref.best_ask(locate)
                      && book->best_bid_depth() == ref.best_bid_depth(locate)
                      && book->best_ask_depth() == ref.best_ask_depth(locate);
    }
    std::printf("final-state check: %s\n", match ? "MATCH" : "MISMATCH");
    if (const OrderBook* b = fast.book(1))
        std::printf("best-price scans: %llu  (avg %.1f steps/scan)\n",
            (unsigned long long)b->scan_count(),
            b->scan_count() ? double(b->scan_steps()) / b->scan_count() : 0.0);
    return match ? 0 : 2;
}
