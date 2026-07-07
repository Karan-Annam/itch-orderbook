// Open-addressing hash table: order_ref -> order state.
//
// ITCH modify messages (Execute/Cancel/Delete/Replace) carry only the 64-bit
// order reference, not the order's price/side. This table answers "what was
// order R?" in O(1). Design points:
//   * Open addressing with linear probing, load factor kept < 0.7.
//   * Thomas Wang's 64-bit integer mix as the hash (~5 ALU ops, low collision).
//   * Structure-of-arrays: keys[] is a dense uint64 array so 8 keys occupy one
//     64-byte cache line, which lets the SIMD probe compare 8 (AVX-512) or 4
//     (AVX2) candidate slots per instruction. The scalar path is the reference.
//   * Deletion uses backward-shift (Knuth 6.4-R) rather than tombstones, so the
//     table never accumulates dead slots and probe chains stay short.
#pragma once

#include "../util/simd.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace ob {

// Thomas Wang 64-bit integer hash.
inline uint64_t hash_order_ref(uint64_t ref) {
    ref = (~ref) + (ref << 21);
    ref = ref ^ (ref >> 24);
    ref = (ref + (ref << 3)) + (ref << 8);
    ref = ref ^ (ref >> 14);
    ref = (ref + (ref << 2)) + (ref << 4);
    ref = ref ^ (ref >> 28);
    ref = ref + (ref << 31);
    return ref;
}

class OrderRefTable {
public:
    struct Value {
        uint32_t price  = 0;
        uint32_t shares = 0;
        uint16_t locate = 0;
        char     side   = 0;
        uint8_t  _pad   = 0;
    };

    // capacity_pow2: number of slots (power of two). 1<<20 ≈ 1M (spec default).
    explicit OrderRefTable(uint32_t capacity_pow2 = (1u << 20)) { init(capacity_pow2); }

    void init(uint32_t capacity_pow2) {
        // round up to power of two
        uint32_t n = 1;
        while (n < capacity_pow2) n <<= 1;
        cap_  = n;
        mask_ = n - 1;
        keys_.assign(n, 0);     // 0 == empty (order_ref 0 is reserved/unused)
        vals_.assign(n, Value{});
        size_ = 0;
        clear_stats();
    }

    size_t size() const { return size_; }
    size_t capacity() const { return cap_; }
    double load_factor() const { return double(size_) / double(cap_); }

    // Insert or overwrite. Returns false only if the table is full.
    bool insert(uint64_t ref, const Value& v) {
        if (ref == 0) return false;          // 0 is the empty sentinel
        uint32_t i = uint32_t(hash_order_ref(ref)) & mask_;
        uint32_t probes = 1;
        while (keys_[i] != 0) {
            if (keys_[i] == ref) { vals_[i] = v; return true; }  // update
            i = (i + 1) & mask_;
            if (++probes > cap_) return false;                   // full
        }
        record_probes(probes);
        keys_[i] = ref;
        vals_[i] = v;
        ++size_;
        return true;
    }

    // Find slot index of `ref`, or -1. SIMD-accelerated probe.
    int64_t find_index(uint64_t ref) const {
        if (ref == 0) return -1;
        uint32_t i = uint32_t(hash_order_ref(ref)) & mask_;
        return probe(ref, i);
    }

    Value* find(uint64_t ref) {
        int64_t idx = find_index(ref);
        return idx < 0 ? nullptr : &vals_[size_t(idx)];
    }
    const Value* find(uint64_t ref) const {
        int64_t idx = find_index(ref);
        return idx < 0 ? nullptr : &vals_[size_t(idx)];
    }

    // Remove `ref` with backward-shift deletion. Returns true if removed.
    bool erase(uint64_t ref) {
        int64_t found = find_index(ref);
        if (found < 0) return false;
        uint32_t i = uint32_t(found);
        uint32_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (keys_[j] == 0) break;
            uint32_t k = uint32_t(hash_order_ref(keys_[j])) & mask_;
            // Skip if k lies cyclically in (i, j] — element is correctly placed.
            const bool in_gap = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (in_gap) continue;
            keys_[i] = keys_[j];
            vals_[i] = vals_[j];
            i = j;
        }
        keys_[i] = 0;
        vals_[i] = Value{};
        --size_;
        return true;
    }

    // ---- collision / probe statistics -------------------------------------
    struct Stats {
        uint64_t probe1 = 0, probe2 = 0, probe_gt2 = 0;
        uint64_t collisions = 0;     // inserts that probed past their home slot
        uint64_t total_inserts = 0;
        double   collision_rate() const {
            return total_inserts ? double(collisions) / double(total_inserts) : 0.0;
        }
    };
    const Stats& stats() const { return stats_; }
    void clear_stats() { stats_ = Stats{}; }

private:
    // Linear-probe search from home slot `start`.
    int64_t probe(uint64_t ref, uint32_t start) const {
#if defined(OB_SIMD_AVX512)
        return probe_avx512(ref, start);
#elif defined(OB_SIMD_AVX2)
        return probe_avx2(ref, start);
#else
        return probe_scalar(ref, start);
#endif
    }

    int64_t probe_scalar(uint64_t ref, uint32_t start) const {
        uint32_t i = start;
        for (uint32_t n = 0; n <= cap_; ++n) {
            uint64_t k = keys_[i];
            if (k == ref) return int64_t(i);
            if (k == 0)   return -1;
            i = (i + 1) & mask_;
        }
        return -1;
    }

#if defined(OB_SIMD_AVX2)
    // Compare 4 keys per 256-bit vector. Handles wraparound by falling back to
    // scalar near the end of the array (rare).
    int64_t probe_avx2(uint64_t ref, uint32_t start) const {
        const __m256i target = _mm256_set1_epi64x(int64_t(ref));
        const __m256i zero   = _mm256_setzero_si256();
        uint32_t i = start;
        uint32_t scanned = 0;
        while (scanned <= cap_) {
            if (i + 4 <= cap_) {
                __m256i k = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(&keys_[i]));
                uint32_t hit  = uint32_t(_mm256_movemask_pd(
                    _mm256_castsi256_pd(_mm256_cmpeq_epi64(k, target))));
                uint32_t zero_m = uint32_t(_mm256_movemask_pd(
                    _mm256_castsi256_pd(_mm256_cmpeq_epi64(k, zero))));
                if (hit) return int64_t(i + __builtin_ctz(hit));
                if (zero_m) return -1;     // empty slot before any match
                i += 4; scanned += 4;
            } else {
                // tail / wraparound: one slot at a time
                uint64_t kk = keys_[i];
                if (kk == ref) return int64_t(i);
                if (kk == 0)   return -1;
                i = (i + 1) & mask_; ++scanned;
            }
        }
        return -1;
    }
#endif

#if defined(OB_SIMD_AVX512)
    int64_t probe_avx512(uint64_t ref, uint32_t start) const {
        const __m512i target = _mm512_set1_epi64(int64_t(ref));
        const __m512i zero   = _mm512_setzero_si512();
        uint32_t i = start;
        uint32_t scanned = 0;
        while (scanned <= cap_) {
            if (i + 8 <= cap_) {
                __m512i k = _mm512_loadu_si512(
                    reinterpret_cast<const void*>(&keys_[i]));
                __mmask8 hit  = _mm512_cmpeq_epi64_mask(k, target);
                __mmask8 zero_m = _mm512_cmpeq_epi64_mask(k, zero);
                if (hit) return int64_t(i + __builtin_ctz(unsigned(hit)));
                if (zero_m) return -1;
                i += 8; scanned += 8;
            } else {
                uint64_t kk = keys_[i];
                if (kk == ref) return int64_t(i);
                if (kk == 0)   return -1;
                i = (i + 1) & mask_; ++scanned;
            }
        }
        return -1;
    }
#endif

    void record_probes(uint32_t probes) {
        ++stats_.total_inserts;
        if (probes == 1)      ++stats_.probe1;
        else if (probes == 2) ++stats_.probe2;
        else                  ++stats_.probe_gt2;
        if (probes > 1) ++stats_.collisions;
    }

    std::vector<uint64_t> keys_;
    std::vector<Value>    vals_;
    uint32_t cap_ = 0, mask_ = 0;
    size_t   size_ = 0;
    Stats    stats_;
};

}  // namespace ob
