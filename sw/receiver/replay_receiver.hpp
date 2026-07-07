// Portable file-replay feed source (the default).
//
// The kernel-bypass receivers (AF_XDP / raw busy-poll) are Linux-only. To keep
// the whole pipeline runnable and testable on any platform, the
// default feed source replays a recorded/synthetic ITCH file from memory. It
// can hand out the whole buffer at once (fastest, for benchmarking the parse +
// book hot path) or in fixed-size "packet" chunks aligned to message
// boundaries (to exercise the UDP/framing path).
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>
#include <string>

#include "../parser/itch_parser.hpp"

namespace ob {

class FileReplayReceiver {
public:
    bool load(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz < 0) { std::fclose(f); return false; }
        buf_.resize(size_t(sz));
        size_t rd = std::fread(buf_.data(), 1, buf_.size(), f);
        std::fclose(f);
        buf_.resize(rd);
        return true;
    }

    void load_bytes(const std::vector<uint8_t>& bytes) { buf_ = bytes; }

    const uint8_t* data() const { return buf_.data(); }
    size_t         size() const { return buf_.size(); }
    bool           empty() const { return buf_.empty(); }

    // Deliver the stream as a sequence of message-boundary-aligned "datagrams"
    // of up to max_bytes each, invoking cb(payload_ptr, payload_len) per packet.
    // This mirrors how ITCH arrives in UDP datagrams carrying several messages.
    template <typename Cb>
    void deliver_packets(size_t max_bytes, Cb&& cb) const {
        size_t o = 0;
        while (o < buf_.size()) {
            size_t start = o;
            size_t pkt = 0;
            while (o + 2 <= buf_.size()) {
                uint16_t blen = be16(buf_.data() + o);
                if (blen == 0) { o = buf_.size(); break; }
                size_t msz = 2 + blen;
                if (o + msz > buf_.size()) { o = buf_.size(); break; }
                if (pkt && pkt + msz > max_bytes) break;   // close this packet
                pkt += msz;
                o   += msz;
            }
            if (pkt == 0) break;
            cb(buf_.data() + start, pkt);
        }
    }

private:
    std::vector<uint8_t> buf_;
};

}  // namespace ob
