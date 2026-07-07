// gen_itch.cpp — command-line tool to materialise a synthetic ITCH 5.0 file.
//
//   gen_itch <out.itch> [num_messages] [num_symbols] [seed]
//
// The produced file is a raw concatenation of length-prefixed ITCH message
// bodies, byte-identical to what a (subset of a) NASDAQ feed would contain.
// Both the C++ pipeline and the RTL Verilator harness replay this same file.
#include "../sw/util/itch_gen.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <out.itch> [num_messages=200000] [num_symbols=4] [seed]\n",
            argv[0]);
        return 2;
    }
    const char* out = argv[1];
    size_t n        = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 200000;
    int symbols     = (argc > 3) ? std::atoi(argv[3]) : 4;
    uint64_t seed   = (argc > 4) ? std::strtoull(argv[4], nullptr, 10) : 0xC0FFEE12345ULL;

    ob::GenConfig cfg;
    cfg.num_symbols = symbols;
    cfg.seed        = seed;

    ob::ItchGenerator gen(cfg);
    std::vector<uint8_t> stream = gen.generate(n);

    if (!ob::ItchGenerator::write_file(out, stream)) {
        std::fprintf(stderr, "error: could not write %s\n", out);
        return 1;
    }

    const ob::GenStats& s = gen.stats();
    std::printf("Wrote %s: %zu bytes, %llu messages, peak live orders=%llu\n",
                out, stream.size(), (unsigned long long)s.total,
                (unsigned long long)s.live_orders_peak);
    static const char* names[] = {"Add","AddMPID","Exec","ExecPrice",
                                  "Cancel","Delete","Replace","Trade","System"};
    for (int i = 0; i < ob::IDX_COUNT; ++i)
        std::printf("  %-10s %llu\n", names[i], (unsigned long long)s.per_type[i]);
    return 0;
}
