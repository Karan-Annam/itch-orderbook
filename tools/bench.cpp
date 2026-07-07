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

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

using namespace ob;

template <typename Fn>
static double time_ms(Fn&& fn) {
    uint64_t t0 = rdtscp();
    fn();
    uint64_t t1 = rdtscp();
    static TscClock clk = [] { TscClock c; c.calibrate(80); return c; }();
    return clk.to_ns(t1 - t0) / 1e6;
}

int main(int argc, char** argv) {
    size_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 500000;
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

    ReferenceBook ref;
    double ms_ref = time_ms([&] { for (auto& m : msgs) ref.apply(m); });

    BookEngine fast(1'000'000, 1u << 21);
    double ms_fast = time_ms([&] { for (auto& m : msgs) fast.apply(m); });

    auto mps = [&](double ms) { return msgs.size() / (ms / 1000.0) / 1e6; };
    auto nsop = [&](double ms) { return ms * 1e6 / msgs.size(); };

    std::printf("%-28s %10s %14s %12s\n", "implementation", "time(ms)", "Mmsg/s", "ns/msg");
    std::printf("%-28s %10.1f %14.2f %12.1f\n",
                "std::map (RB-tree)", ms_ref, mps(ms_ref), nsop(ms_ref));
    std::printf("%-28s %10.1f %14.2f %12.1f\n",
                "direct-index + SIMD", ms_fast, mps(ms_fast), nsop(ms_fast));
    std::printf("\nSpeedup: %.2fx\n", ms_ref / ms_fast);

    if (const OrderBook* b = fast.book(1))
        std::printf("best-price scans: %llu  (avg %.1f steps/scan)\n",
            (unsigned long long)b->scan_count(),
            b->scan_count() ? double(b->scan_steps()) / b->scan_count() : 0.0);
    return 0;
}
