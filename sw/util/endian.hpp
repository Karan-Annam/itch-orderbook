// endian.hpp — big-endian (network byte order) load helpers.
//
// ITCH 5.0 stores every multi-byte integer in big-endian order. x86 is
// little-endian, so every field must be byte-swapped on load. The compiler
// lowers __builtin_bswapNN to a single BSWAP/MOVBE instruction.
//
// We read from a (possibly unaligned) byte pointer via memcpy, which the
// compiler turns into a single load — this is the standard, strict-aliasing
// safe idiom and is exactly as fast as a reinterpret_cast on x86.
#pragma once

#include <cstdint>
#include <cstring>

namespace ob {

// ---- raw unaligned loads (host byte order) --------------------------------
inline uint16_t load_u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline uint32_t load_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline uint64_t load_u64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// ---- big-endian (network order) loads -------------------------------------
inline uint16_t be16(const uint8_t* p) { return __builtin_bswap16(load_u16(p)); }
inline uint32_t be32(const uint8_t* p) { return __builtin_bswap32(load_u32(p)); }
inline uint64_t be64(const uint8_t* p) { return __builtin_bswap64(load_u64(p)); }

// ITCH timestamps are 48-bit (6 bytes) big-endian: nanoseconds since midnight.
inline uint64_t be48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}

// ---- big-endian stores (used by the synthetic generator) ------------------
inline void store_be16(uint8_t* p, uint16_t v) {
    v = __builtin_bswap16(v);
    std::memcpy(p, &v, sizeof(v));
}
inline void store_be32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, sizeof(v));
}
inline void store_be64(uint8_t* p, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(p, &v, sizeof(v));
}
inline void store_be48(uint8_t* p, uint64_t v) {
    for (int i = 5; i >= 0; --i) { p[i] = uint8_t(v & 0xFF); v >>= 8; }
}

}  // namespace ob
