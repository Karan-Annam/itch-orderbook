// mold_build.hpp — MoldUDP64 packet builders for the RTL tests.
//
// A mold packet is: session[10] | sequence[8] BE | count[2] BE, followed by
// `count` message blocks. Each block is [2-byte BE length][body] — exactly the
// project's length-prefixed message format, so itch_build.hpp outputs are used
// as blocks unchanged. udp_wrap() prepends the 42 dummy Ethernet/IPv4/UDP
// header bytes the udp_stripper discards (content ignored, 0xEE like
// test_udp_strip).
#pragma once

#include <cstdint>
#include <vector>

namespace obsim {

inline std::vector<uint8_t> mold_header(uint64_t seq, uint16_t count) {
    std::vector<uint8_t> h(20, uint8_t('S'));            // session: don't care
    for (int i = 0; i < 8; ++i) h[10 + i] = uint8_t(seq >> (8 * (7 - i)));
    h[18] = uint8_t(count >> 8);
    h[19] = uint8_t(count & 0xFF);
    return h;
}

// packet carrying the given pre-framed messages (each already [len][body])
inline std::vector<uint8_t> mold_packet(uint64_t seq,
                                        const std::vector<std::vector<uint8_t>>& msgs) {
    std::vector<uint8_t> p = mold_header(seq, uint16_t(msgs.size()));
    for (const auto& m : msgs) p.insert(p.end(), m.begin(), m.end());
    return p;
}

inline std::vector<uint8_t> mold_heartbeat(uint64_t seq) {
    return mold_header(seq, 0);
}

inline std::vector<uint8_t> mold_session_end(uint64_t seq) {
    return mold_header(seq, 0xFFFF);
}

inline std::vector<uint8_t> udp_wrap(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f(42, 0xEE);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

}  // namespace obsim
