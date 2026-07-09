// rtl_driver.hpp — thin wrapper that clocks Vorderbook_top and feeds a raw ITCH
// stream 16 bytes per cycle with ready/valid backpressure. A per-commit
// callback fires once per fully-applied message (detected via the msg_total
// counter), in stream order, so the harness can diff RTL state against the
// reference model.
#pragma once

#include "Vorderbook_top.h"
#include "verilated.h"

#include <algorithm>
#include <cstdint>
#include <vector>
#include <functional>

namespace obsim {

// Ingest bus geometry — must match WORD_BYTES in rtl/ob_pkg.sv.
static constexpr size_t WORD_BYTES = 16;

// Load one input beat: n bytes (1..16) packed from lane 0. in_data is 128-bit,
// a VlWide<4> (uint32_t[4]) on the Verilated model.
inline void set_in_word(Vorderbook_top* t, const uint8_t* p, size_t n) {
    for (int w = 0; w < 4; ++w) t->in_data[w] = 0;
    for (size_t i = 0; i < n; ++i)
        t->in_data[i >> 2] |= uint32_t(p[i]) << (8 * (i & 3));
    t->in_nbytes = static_cast<uint8_t>(n);
}

class RtlDriver {
public:
    RtlDriver() : top_(new Vorderbook_top) {}
    ~RtlDriver() { top_->final(); delete top_; }

    Vorderbook_top* top() { return top_; }
    uint64_t cycles() const { return cyc_; }

    void tick() {
        top_->clk = 0; top_->eval();
        top_->clk = 1; top_->eval();
        ++cyc_;
    }

    void reset(bool raw_mode = true) {
        top_->rst = 1; top_->in_valid = 0; top_->in_sop = 0;
        for (int w = 0; w < 4; ++w) top_->in_data[w] = 0;
        top_->in_nbytes = 0;
        top_->raw_mode = raw_mode ? 1 : 0;
        top_->dbg_is_bid = 0; top_->dbg_price = 0;
        for (int i = 0; i < 6; ++i) tick();
        top_->rst = 0;
    }

    // Drive the stream. on_commit(index) is called after the Nth message has
    // been applied in the RTL. Returns the number of messages committed.
    // Aborts (returns early) if the cycle cap is exceeded — a hang is a failure.
    uint64_t drive(const std::vector<uint8_t>& s, uint64_t expected,
                   const std::function<void(uint64_t)>& on_commit,
                   uint64_t cycle_cap = 200000000ULL) {
        size_t   i = 0;
        uint64_t committed = 0;
        uint64_t last_total = top_->msg_total;
        while (committed < expected && cyc_ < cycle_cap) {
            if (top_->in_ready && i < s.size()) {
                size_t n = std::min(WORD_BYTES, s.size() - i);
                set_in_word(top_, &s[i], n);
                top_->in_valid = 1;
                i += n;
            } else {
                top_->in_valid = 0;
            }
            tick();
            if (top_->msg_total != last_total) {
                // one or more commits this cycle (ev_done is one-per-cycle, so
                // msg_total increases by exactly 1)
                last_total = top_->msg_total;
                on_commit(committed);
                ++committed;
            }
        }
        return committed;
    }

    // Drive with per-message boundary tracking so the harness can separate
    // *ingest time* (16-byte-per-cycle word feed) from *pipeline latency*
    // (decode + book update + best tracker). frame_len[k] = bytes of message k
    // in the stream (2-byte length prefix + body). For each commit (FIFO
    // order) the callback receives:
    //   idx              message index
    //   ingest_first_cyc cycle the word carrying the message's FIRST byte was
    //                    clocked in (end-to-end latency = commit - this)
    //   ingest_done_cyc  cycle the word carrying the message's LAST byte was
    //                    clocked in
    //   commit_cyc       cycle the message committed (ev_done)
    // so pipeline latency = commit_cyc - ingest_done_cyc, and service time
    // (throughput) = commit_cyc - previous commit_cyc. Ingest is tracked at
    // word granularity: one word may carry the tail of message k and the head
    // of message k+1 (never more — the minimum frame equals the word size).
    uint64_t drive_latency(
            const std::vector<uint8_t>& s,
            const std::vector<size_t>& frame_len,
            const std::function<void(uint64_t, uint64_t, uint64_t, uint64_t)>& on_msg,
            uint64_t cycle_cap = 200000000ULL) {
        const size_t nmsgs = frame_len.size();
        // per-message [start, end) byte ranges in the stream
        std::vector<size_t> msg_start(nmsgs, 0), msg_end(nmsgs, 0);
        {
            size_t pos = 0;
            for (size_t k = 0; k < nmsgs; ++k) {
                msg_start[k] = pos;
                pos += frame_len[k];
                msg_end[k] = pos;
            }
        }
        std::vector<uint64_t> ingest_first(nmsgs, 0), ingest_done(nmsgs, 0);
        size_t   i = 0;
        size_t   first_ptr = 0, done_ptr = 0;
        uint64_t committed = 0;
        uint64_t last_total = top_->msg_total;
        while (committed < nmsgs && cyc_ < cycle_cap) {
            bool   presented = false;
            size_t n = 0;
            if (top_->in_ready && i < s.size()) {
                n = std::min(WORD_BYTES, s.size() - i);
                set_in_word(top_, &s[i], n);
                top_->in_valid = 1;
                presented = true;
            } else {
                top_->in_valid = 0;
            }
            tick();
            if (presented) {
                // this word carried stream bytes [i, i+n)
                while (first_ptr < nmsgs && msg_start[first_ptr] < i + n)
                    ingest_first[first_ptr++] = cyc_;
                while (done_ptr < nmsgs && msg_end[done_ptr] <= i + n)
                    ingest_done[done_ptr++] = cyc_;
                i += n;
            }
            if (top_->msg_total != last_total) {
                last_total = top_->msg_total;
                on_msg(committed, ingest_first[committed], ingest_done[committed], cyc_);
                ++committed;
            }
        }
        return committed;
    }

    // Query a price level via the debug port (combinational async read).
    uint32_t level_shares(bool is_bid, uint32_t price) {
        top_->dbg_is_bid = is_bid ? 1 : 0;
        top_->dbg_price  = price;
        top_->eval();
        return top_->dbg_shares;
    }
    uint32_t level_count(bool is_bid, uint32_t price) {
        top_->dbg_is_bid = is_bid ? 1 : 0;
        top_->dbg_price  = price;
        top_->eval();
        return top_->dbg_count;
    }

private:
    Vorderbook_top* top_;
    uint64_t        cyc_ = 0;
};

}  // namespace obsim
