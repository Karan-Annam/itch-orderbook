// order_ref_table.cpp — out-of-line diagnostics for the order reference table.
// The hot paths (insert/find/erase) are header-inline; this TU holds reporting
// helpers so they do not bloat the hot translation units.
#include "order_ref_table.hpp"

#include <cstdio>

namespace ob {

void write_hash_stats_csv(const char* path, const OrderRefTable& t) {
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    const auto& s = t.stats();
    std::fprintf(f, "metric,value\n");
    std::fprintf(f, "capacity,%zu\n", t.capacity());
    std::fprintf(f, "size,%zu\n", t.size());
    std::fprintf(f, "load_factor,%.4f\n", t.load_factor());
    std::fprintf(f, "probe1,%llu\n",    (unsigned long long)s.probe1);
    std::fprintf(f, "probe2,%llu\n",    (unsigned long long)s.probe2);
    std::fprintf(f, "probe_gt2,%llu\n", (unsigned long long)s.probe_gt2);
    std::fprintf(f, "collisions,%llu\n",(unsigned long long)s.collisions);
    std::fprintf(f, "collision_rate,%.6f\n", s.collision_rate());
    std::fclose(f);
}

}  // namespace ob
