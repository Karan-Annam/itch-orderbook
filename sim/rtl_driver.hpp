// rtl_driver.hpp — thin wrapper that clocks Vorderbook_top and feeds a raw ITCH
// byte stream with ready/valid backpressure. A per-commit callback fires once
// per fully-applied message (detected via the msg_total counter), in stream
// order, so the harness can diff RTL state against the reference model.
#pragma once

#include "Vorderbook_top.h"
#include "verilated.h"

#include <cstdint>
#include <vector>
#include <functional>

namespace obsim {

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
                top_->in_byte  = s[i++];
                top_->in_valid = 1;
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
    // *ingest time* (serial 1-byte/cycle feed) from *pipeline latency* (decode
    // + book update + best tracker). frame_len[k] = bytes of message k in the
    // stream (2-byte length prefix + body). For each commit (FIFO order) the
    // callback receives:
    //   idx             message index
    //   ingest_done_cyc cycle the message's LAST byte was clocked in
    //   commit_cyc      cycle the message committed (ev_done)
    // so true pipeline latency = commit_cyc - ingest_done_cyc, and service time
    // (throughput) = commit_cyc - previous commit_cyc.
    uint64_t drive_latency(
            const std::vector<uint8_t>& s,
            const std::vector<size_t>& frame_len,
            const std::function<void(uint64_t, uint64_t, uint64_t)>& on_msg,
            uint64_t cycle_cap = 200000000ULL) {
        const size_t nmsgs = frame_len.size();
        std::vector<uint64_t> ingest_done(nmsgs, 0);
        size_t   i = 0;
        size_t   cur_msg  = 0;
        size_t   boundary = nmsgs ? frame_len[0] : 0;  // exclusive end of cur msg
        uint64_t committed = 0;
        uint64_t last_total = top_->msg_total;
        while (committed < nmsgs && cyc_ < cycle_cap) {
            bool   presented = false;
            size_t presented_idx = i;
            if (top_->in_ready && i < s.size()) {
                top_->in_byte  = s[i];
                top_->in_valid = 1;
                presented = true;
                ++i;
            } else {
                top_->in_valid = 0;
            }
            tick();
            // Was that the final byte of the current message? Record its cycle.
            if (presented && cur_msg < nmsgs && presented_idx + 1 == boundary) {
                ingest_done[cur_msg] = cyc_;
                ++cur_msg;
                if (cur_msg < nmsgs) boundary += frame_len[cur_msg];
            }
            if (top_->msg_total != last_total) {
                last_total = top_->msg_total;
                on_msg(committed, ingest_done[committed], cyc_);
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
