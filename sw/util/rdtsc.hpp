// rdtsc.hpp — TSC (Time-Stamp Counter) based nanosecond timing.
//
// On modern x86 with an invariant TSC (constant_tsc / nonstop_tsc), RDTSC ticks
// at a fixed frequency independent of the core's P-state, which makes it the
// lowest-overhead high-resolution clock available (a handful of cycles, no
// syscall). We calibrate ticks-per-nanosecond once at startup against
// std::chrono::steady_clock.
#pragma once

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <limits>
#include <thread>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace ob {

struct TscSample {
    uint64_t ticks;
    uint32_t aux;
};

// RDTSCP reports the logical processor in TSC_AUX. LFENCE on both sides keeps
// surrounding loads/instructions out of the measured region; the compiler
// memory clobber prevents source-level motion across the sample.
inline TscSample tsc_sample() {
#if defined(__GNUC__) || defined(__clang__)
    uint32_t lo, hi, aux;
    __asm__ __volatile__("lfence\n\trdtscp\n\tlfence"
                         : "=a"(lo), "=d"(hi), "=c"(aux)
                         : : "memory");
    return {(static_cast<uint64_t>(hi) << 32) | lo, aux};
#else
    unsigned aux;
    _mm_lfence();
    const uint64_t ticks = __rdtscp(&aux);
    _mm_lfence();
    return {ticks, aux};
#endif
}

inline uint64_t tsc_sample_overhead(unsigned samples = 1000) {
    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (unsigned i = 0; i < samples; ++i) {
        const TscSample a = tsc_sample();
        const TscSample b = tsc_sample();
        if (a.aux == b.aux && b.ticks >= a.ticks)
            best = std::min(best, b.ticks - a.ticks);
    }
    return best == std::numeric_limits<uint64_t>::max() ? 0 : best;
}

// Calibrate ticks-per-nanosecond by sleeping a known wall-clock interval.
// Returns ticks/ns (e.g. ~3.0 on a 3 GHz invariant TSC).
inline double calibrate_tsc(int sample_ms = 50) {
    using clock = std::chrono::steady_clock;
    const TscSample t0 = tsc_sample();
    const auto     w0 = clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));
    const TscSample t1 = tsc_sample();
    const auto     w1 = clock::now();
    const double ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count();
    const double ticks = static_cast<double>(t1.ticks - t0.ticks);
    return ticks / ns;  // ticks per nanosecond
}

// A calibrated converter held by value so the hot path never touches globals.
struct TscClock {
    double ticks_per_ns = 1.0;

    TscClock() = default;
    void calibrate(int sample_ms = 50) { ticks_per_ns = calibrate_tsc(sample_ms); }

    // Convert an elapsed tick count into nanoseconds.
    double to_ns(uint64_t ticks) const {
        return static_cast<double>(ticks) / ticks_per_ns;
    }
    uint64_t to_ns_u64(uint64_t ticks) const {
        return static_cast<uint64_t>(static_cast<double>(ticks) / ticks_per_ns);
    }
};

}  // namespace ob
