// Portable SIMD feature detection and small helpers.
//
// AVX-512 would be ideal (e.g. _mm512_cmpeq_epi8_mask) but plenty of machines —
// including my dev box — have AVX2 and not AVX-512. To keep one source tree
// that's fast where AVX-512 exists, fast on AVX2, and correct everywhere, every
// SIMD routine in this codebase is written with three compile-time-selected
// implementations that return identical results:
//
//     OB_SIMD_AVX512   — best path, 64-byte / 8-lane ops
//     OB_SIMD_AVX2     — 32-byte / 8-lane ops
//     (scalar)         — portable fallback, always correct
//
// Select with the OB_HAVE_* macros below. The build picks the widest ISA the
// host advertises (`-march=native`); the scalar path is the reference used by
// the unit tests to validate the vector paths bit-for-bit.
#pragma once

#if defined(__AVX512F__) && defined(__AVX512BW__)
#  define OB_SIMD_AVX512 1
#  include <immintrin.h>
#elif defined(__AVX2__)
#  define OB_SIMD_AVX2 1
#  include <immintrin.h>
#endif

#include <cstdint>

namespace ob {

// Human-readable description of the SIMD tier actually compiled in.
inline const char* simd_tier() {
#if defined(OB_SIMD_AVX512)
    return "AVX-512 (cmpeq_epi8_mask, 64B)";
#elif defined(OB_SIMD_AVX2)
    return "AVX2 (cmpeq_epi8, 32B)";
#else
    return "scalar";
#endif
}

}  // namespace ob
