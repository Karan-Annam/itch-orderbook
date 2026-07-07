// rdtsc.hpp — TSC (Time-Stamp Counter) based nanosecond timing.
//
// On modern x86 with an invariant TSC (constant_tsc / nonstop_tsc), RDTSC ticks
// at a fixed frequency independent of the core's P-state, which makes it the
// lowest-overhead high-resolution clock available (a handful of cycles, no
// syscall). We calibrate ticks-per-nanosecond once at startup against
// std::chrono::steady_clock.
#pragma once

#include <cstdint>
#include <chrono>
#include <thread>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace ob {

// Read the time-stamp counter. Compiles to a single RDTSC instruction.
inline uint64_t rdtsc() {
#if defined(__GNUC__) || defined(__clang__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return __rdtsc();
#endif
}

// Serialising variant: RDTSCP waits for prior instructions to retire, so it is
// the correct fence to bracket a measured region (avoids out-of-order leakage).
inline uint64_t rdtscp() {
#if defined(__GNUC__) || defined(__clang__)
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    unsigned aux;
    return __rdtscp(&aux);
#endif
}

// Calibrate ticks-per-nanosecond by sleeping a known wall-clock interval.
// Returns ticks/ns (e.g. ~3.0 on a 3 GHz invariant TSC).
inline double calibrate_tsc(int sample_ms = 50) {
    using clock = std::chrono::steady_clock;
    const uint64_t t0 = rdtscp();
    const auto     w0 = clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));
    const uint64_t t1 = rdtscp();
    const auto     w1 = clock::now();
    const double ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count();
    const double ticks = static_cast<double>(t1 - t0);
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
