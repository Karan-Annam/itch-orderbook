// ITCH 5.0 decoder: raw bytes -> DecodedMessage.
//
// Two layers:
//   * UDP/IP stripper: skip the 42-byte Ethernet+IPv4+UDP header to reach the
//     ITCH payload (for the packet path).
//   * ITCH decoder: walk the length-prefixed messages in a payload, byte-swap
//     each big-endian field, and fill a DecodedMessage.
//
// The decode hot path is header-inline. A SIMD batch header scan (used to
// validate framing / locate type bytes across 64 bytes at once) lives in the
// .cpp alongside the UDP strip.
#pragma once

#include "itch_messages.hpp"
#include "../util/endian.hpp"

#include <cstdint>
#include <cstddef>

namespace ob {

// Offset from the start of a (non-fragmented IPv4) Ethernet frame to the ITCH
// payload: 14 (Ethernet) + 20 (IPv4, no options) + 8 (UDP) = 42.
constexpr size_t UDP_PAYLOAD_OFFSET = 42;

struct ParseStats {
    uint64_t messages = 0;
    uint64_t bytes    = 0;
    uint64_t unknown  = 0;
};

class ItchParser {
public:
    // Decode one message body (without the 2-byte length prefix).
    static DecodedMessage decode_body(const uint8_t* b, size_t body_len);

    // Walk a raw length-prefixed ITCH stream, invoking handler(const DecodedMessage&)
    // for each message. Returns parse stats. `handler` may be any callable.
    template <typename Handler>
    static ParseStats parse_stream(const uint8_t* data, size_t len, Handler&& handler) {
        ParseStats st;
        size_t o = 0;
        while (o + 2 <= len) {
            uint16_t body_len = be16(data + o);
            if (body_len == 0 || o + 2 + body_len > len) break;
            const uint8_t* body = data + o + 2;
            DecodedMessage m = decode_body(body, body_len);
            if (m.type == MsgType::Unknown) ++st.unknown;
            handler(m);
            ++st.messages;
            st.bytes += 2 + body_len;
            o += 2 + body_len;
        }
        return st;
    }

    // Strip Ethernet/IPv4/UDP headers; return pointer/len of ITCH payload.
    // Returns false if the frame is too short.
    static bool strip_udp(const uint8_t* frame, size_t frame_len,
                          const uint8_t** payload, size_t* payload_len) {
        if (frame_len <= UDP_PAYLOAD_OFFSET) return false;
        *payload     = frame + UDP_PAYLOAD_OFFSET;
        *payload_len = frame_len - UDP_PAYLOAD_OFFSET;
        return true;
    }

    // SIMD batch scan: count occurrences of `type_byte` in [data, data+len).
    // Used by tests to cross-check framing. Defined in itch_parser.cpp.
    static size_t simd_count_type(const uint8_t* data, size_t len, uint8_t type_byte);

private:
    static void copy8(char* dst, const uint8_t* src) {
        for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>(src[i]);
    }
};

}  // namespace ob
