// Tiny ITCH message builders for the focused unit tests. Produces
// length-prefixed bytes at the exact field offsets, so tests can hand-craft
// precise scenarios (force a best-price scan, a mid-chain delete, a Replace).
#pragma once

#include <cstdint>
#include <vector>

namespace obsim {

inline void put_be16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    v[off] = uint8_t(x >> 8); v[off+1] = uint8_t(x);
}
inline void put_be32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off]=uint8_t(x>>24); v[off+1]=uint8_t(x>>16); v[off+2]=uint8_t(x>>8); v[off+3]=uint8_t(x);
}
inline void put_be64(std::vector<uint8_t>& v, size_t off, uint64_t x) {
    for (int i=0;i<8;++i) v[off+i]=uint8_t(x >> (56-8*i));
}

// Build a length-prefixed message body of `blen` bytes with common header set.
inline std::vector<uint8_t> base_msg(size_t blen, char type, uint16_t locate=1) {
    std::vector<uint8_t> m(2 + blen, 0);
    put_be16(m, 0, uint16_t(blen));        // length prefix
    m[2 + 0] = uint8_t(type);              // body[0] = type
    put_be16(m, 2 + 1, locate);            // stock_locate
    return m;
}

inline std::vector<uint8_t> add(uint64_t ref, char side, uint32_t shares, uint32_t price,
                                uint16_t locate=1) {
    auto m = base_msg(38, 'A', locate); uint8_t* b = m.data() + 2;
    put_be64(m, 2+13, ref); b[21]=uint8_t(side); put_be32(m,2+22,shares); put_be32(m,2+34,price);
    return m;
}
inline std::vector<uint8_t> exec(uint64_t ref, uint32_t shares) {
    auto m = base_msg(33, 'E'); put_be64(m,2+13,ref); put_be32(m,2+21,shares);
    put_be64(m,2+25, 7); return m;
}
inline std::vector<uint8_t> exec_price(uint64_t ref, uint32_t shares, uint32_t price, bool pr=true) {
    auto m = base_msg(38, 'C'); put_be64(m,2+13,ref); put_be32(m,2+21,shares);
    put_be64(m,2+25,7); m[2+33]= pr?'Y':'N'; put_be32(m,2+34,price); return m;
}
inline std::vector<uint8_t> cancel(uint64_t ref, uint32_t shares) {
    auto m = base_msg(25, 'X'); put_be64(m,2+13,ref); put_be32(m,2+21,shares); return m;
}
inline std::vector<uint8_t> del(uint64_t ref) {
    auto m = base_msg(21, 'D'); put_be64(m,2+13,ref); return m;
}
inline std::vector<uint8_t> replace(uint64_t oref, uint64_t nref, uint32_t shares, uint32_t price) {
    auto m = base_msg(37, 'U'); put_be64(m,2+13,oref); put_be64(m,2+21,nref);
    put_be32(m,2+29,shares); put_be32(m,2+33,price); return m;
}
inline std::vector<uint8_t> trade(char side, uint32_t shares, uint32_t price) {
    auto m = base_msg(46, 'P'); put_be64(m,2+13,0); m[2+21]=uint8_t(side);
    put_be32(m,2+22,shares); put_be32(m,2+34,price); put_be64(m,2+38,9); return m;
}
inline std::vector<uint8_t> sysevt(char code) {
    auto m = base_msg(14, 'S'); m[2+13]=uint8_t(code); return m;
}

inline void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& m) {
    dst.insert(dst.end(), m.begin(), m.end());
}

}  // namespace obsim
