// SIMD batch header scan (out-of-line). Counts occurrences of a byte value
// using the widest vector ISA available: AVX-512 does 64 bytes/iteration via
// _mm512_cmpeq_epi8_mask, AVX2 does 32, scalar otherwise. All three return the
// same count; the scalar path is the reference.
#include "itch_parser.hpp"
#include "../util/simd.hpp"

namespace ob {

size_t ItchParser::simd_count_type(const uint8_t* data, size_t len, uint8_t tb) {
    size_t count = 0;
    size_t i = 0;

#if defined(OB_SIMD_AVX512)
    const __m512i needle = _mm512_set1_epi8(static_cast<char>(tb));
    for (; i + 64 <= len; i += 64) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(data + i));
        __mmask64 hits = _mm512_cmpeq_epi8_mask(v, needle);
        count += __builtin_popcountll(static_cast<unsigned long long>(hits));
    }
#elif defined(OB_SIMD_AVX2)
    const __m256i needle = _mm256_set1_epi8(static_cast<char>(tb));
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        unsigned m = static_cast<unsigned>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, needle)));
        count += static_cast<size_t>(__builtin_popcount(m));
    }
#endif
    for (; i < len; ++i) if (data[i] == tb) ++count;
    return count;
}

}  // namespace ob
