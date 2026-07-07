// spsc_queue.hpp — lock-free single-producer / single-consumer ring buffer.
//
// Used between the receiver thread (producer) and the parser thread (consumer).
// No locks, no atomic read-modify-write — just a release/acquire handshake on a
// pair of indices. The two indices live on separate 64-byte cache lines so the
// producer's store to write_idx never invalidates the consumer's cache line
// holding read_idx (false sharing — the same effect measured in the MESI
// project's false-sharing benchmark).
//
// Correctness on x86-TSO needs only release on publish and acquire on observe;
// no MFENCE. memory_order_relaxed on a thread's own index is sufficient because
// each index has exactly one writer.
#pragma once

#include <atomic>
#include <cstddef>
#include <new>

namespace ob {

template <typename T, size_t N>
struct alignas(64) SPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

    bool push(const T& val) {
        const size_t w = write_idx_.load(std::memory_order_relaxed);
        const size_t r = read_idx_.load(std::memory_order_acquire);
        if (w - r >= N) return false;  // full
        slots_[w & (N - 1)] = val;
        write_idx_.store(w + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t r = read_idx_.load(std::memory_order_relaxed);
        const size_t w = write_idx_.load(std::memory_order_acquire);
        if (r == w) return false;  // empty
        out = slots_[r & (N - 1)];
        read_idx_.store(r + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        return write_idx_.load(std::memory_order_acquire) -
               read_idx_.load(std::memory_order_acquire);
    }
    bool empty() const { return size() == 0; }
    static constexpr size_t capacity() { return N; }

private:
    alignas(64) std::atomic<size_t> write_idx_{0};
    alignas(64) std::atomic<size_t> read_idx_{0};
    alignas(64) T                   slots_[N];
};

}  // namespace ob
