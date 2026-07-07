// latency_hist.hpp — fixed-bin latency histogram with percentile queries.
//
// Bins are 1 ns wide over [0, RANGE) ns, plus one overflow bucket. Recording a
// sample is a single array increment (branchless on the common path), so the
// histogram can be updated on the measured hot path without perturbing it.
// Percentiles (p50/p95/p99/p99.9/p99.99) are computed on demand from the CDF.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ob {

class LatencyHist {
public:
    explicit LatencyHist(uint32_t range_ns = 10000)
        : range_(range_ns), bins_(range_ns + 1, 0) {}

    void record(double ns) {
        if (ns < 0) ns = 0;
        uint32_t idx = (ns >= range_) ? range_ : static_cast<uint32_t>(ns);
        ++bins_[idx];
        ++count_;
        sum_ns_ += ns;
        if (ns > max_ns_) max_ns_ = ns;
        if (ns < min_ns_) min_ns_ = ns;
    }

    uint64_t count() const { return count_; }
    double   mean()  const { return count_ ? sum_ns_ / count_ : 0.0; }
    double   max()   const { return count_ ? max_ns_ : 0.0; }
    double   min()   const { return count_ ? min_ns_ : 0.0; }

    // Smallest latency b such that fraction p of samples are <= b.
    double percentile(double p) const {
        if (count_ == 0) return 0.0;
        const uint64_t target =
            static_cast<uint64_t>(std::ceil(p * static_cast<double>(count_)));
        uint64_t cum = 0;
        for (uint32_t i = 0; i <= range_; ++i) {
            cum += bins_[i];
            if (cum >= target) return static_cast<double>(i);
        }
        return static_cast<double>(range_);
    }

    void merge(const LatencyHist& o) {
        for (uint32_t i = 0; i <= range_ && i <= o.range_; ++i) bins_[i] += o.bins_[i];
        count_  += o.count_;
        sum_ns_ += o.sum_ns_;
        max_ns_  = std::max(max_ns_, o.max_ns_);
        if (o.count_) min_ns_ = std::min(min_ns_, o.min_ns_);
    }

    void print(const char* label) const {
        std::printf(
            "  %-22s n=%-9llu  p50=%6.0f  p95=%6.0f  p99=%6.0f  p99.9=%6.0f  "
            "p99.99=%6.0f  max=%7.0f  mean=%6.1f  (ns)\n",
            label, (unsigned long long)count_, percentile(0.50), percentile(0.95),
            percentile(0.99), percentile(0.999), percentile(0.9999), max(), mean());
    }

    // Dump nonzero bins to a CSV file for the analysis scripts.
    void write_csv(const char* path, const char* type_label) const {
        FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::fprintf(f, "msg_type,latency_ns,count\n");
        for (uint32_t i = 0; i <= range_; ++i)
            if (bins_[i]) std::fprintf(f, "%s,%u,%llu\n", type_label, i,
                                       (unsigned long long)bins_[i]);
        std::fclose(f);
    }

private:
    uint32_t              range_;
    std::vector<uint64_t> bins_;
    uint64_t              count_  = 0;
    double                sum_ns_ = 0.0;
    double                max_ns_ = 0.0;
    double                min_ns_ = 1e18;
};

}  // namespace ob
