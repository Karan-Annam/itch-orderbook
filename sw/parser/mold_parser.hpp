// MoldUDP64 walker: session[10] | seq[8] BE | count[2] BE, then `count`
// blocks of [2-byte BE length][ITCH body]. A .mold file is a concatenation of
// packets; packets are self-delimiting (the header's count bounds the block
// walk), so no outer framing is needed.
//
// Sequence policy mirrors rtl/mold_stripper.sv exactly (the RTL test and
// test_mold_parser assert the same vectors):
//   seq == expected : apply; expected += count (heartbeat count=0 is a no-op)
//   seq >  expected : gap; count events + lost msgs, accept from here
//   seq <  expected : stale/duplicate; skip the WHOLE packet, expected kept
//   count == 0xFFFF : end of session; stop
// First packet seeds `expected` without counting a gap. Detection only — a
// retransmit request path would need a live socket/session, out of scope.
#pragma once

#include "itch_parser.hpp"

#include <vector>

namespace ob {

struct MoldStats {
    uint64_t packets       = 0;
    uint64_t heartbeats    = 0;
    uint64_t messages      = 0;   // applied (stale packets contribute 0)
    uint64_t gap_events    = 0;
    uint64_t gap_msgs      = 0;
    uint64_t stale_packets = 0;
    uint64_t next_seq      = 0;
    bool     seq_init      = false;
    bool     session_end   = false;
    bool     truncated     = false;  // stream ended mid-packet
};

class MoldParser {
public:
    static constexpr size_t HDR_BYTES = 20;

    // Walk concatenated mold packets, invoking handler(const DecodedMessage&)
    // for each applied message.
    template <typename Handler>
    static MoldStats parse_stream(const uint8_t* data, size_t len, Handler&& handler) {
        return walk(data, len, [&](const uint8_t* block, size_t nbytes) {
            handler(ItchParser::decode_body(block + 2, nbytes - 2));
        });
    }

    // Decap to a plain length-prefixed .itch byte stream (applied blocks only,
    // stale packets skipped) — lets the untouched replay/latency path consume
    // mold input.
    static MoldStats decap(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
        return walk(data, len, [&](const uint8_t* block, size_t nbytes) {
            out.insert(out.end(), block, block + nbytes);
        });
    }

private:
    // Core packet walk; block_fn(block_ptr, nbytes) sees each APPLIED block
    // (including its 2-byte length prefix).
    template <typename BlockFn>
    static MoldStats walk(const uint8_t* data, size_t len, BlockFn&& block_fn) {
        MoldStats st;
        size_t o = 0;
        while (o + HDR_BYTES <= len) {
            const uint64_t seq = be64(data + o + 10);
            const uint16_t cnt = be16(data + o + 18);
            o += HDR_BYTES;
            ++st.packets;

            if (cnt == 0xFFFF) { st.session_end = true; break; }

            const bool stale = st.seq_init && (seq < st.next_seq);
            if (st.seq_init && seq > st.next_seq) {
                ++st.gap_events;
                st.gap_msgs += seq - st.next_seq;
            }

            // walk the blocks even when stale — the packet must be consumed
            // whole to stay aligned with the next header
            for (uint16_t i = 0; i < cnt; ++i) {
                if (o + 2 > len) { st.truncated = true; return st; }
                const uint16_t blen_ = be16(data + o);
                if (blen_ == 0 || o + 2 + blen_ > len) { st.truncated = true; return st; }
                if (!stale) {
                    block_fn(data + o, size_t(2 + blen_));
                    ++st.messages;
                }
                o += 2 + blen_;
            }

            if (stale)         ++st.stale_packets;
            else { st.next_seq = seq + cnt; st.seq_init = true; }
            if (cnt == 0)      ++st.heartbeats;
        }
        return st;
    }
};

}  // namespace ob
