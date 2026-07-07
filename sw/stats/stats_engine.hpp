// Running market statistics.
//
// Updated incrementally as messages flow through the pipeline:
//   * VWAP            — volume-weighted average price, fed by E/C/P trade prints
//   * Trade count     — number of executed trades since open
//   * Message rate    — rolling 1-second window plus session average (event time)
//   * Spread          — best_ask - best_bid (ticks), computed from the book
//   * Imbalance       — (bid_vol - ask_vol)/(bid_vol + ask_vol) over top-5 levels
//   * Top-of-book depth — shares at best bid / best ask
//
// 64-bit accumulators avoid overflow over a full trading day.
#pragma once

#include "../parser/itch_messages.hpp"
#include "../book/order_book.hpp"

#include <cstdint>
#include <deque>

namespace ob {

class StatsEngine {
public:
    // Feed one message plus the trade info the book engine resolved for it.
    void on_message(const DecodedMessage& m, const BookEngine::ExecResult& ex);

    // ---- VWAP -------------------------------------------------------------
    uint64_t vwap_numerator()   const { return vwap_num_; }
    uint64_t vwap_denominator() const { return vwap_den_; }
    // VWAP in 1/10000-dollar units (price space); 0 if no volume yet.
    uint32_t vwap_units() const { return vwap_den_ ? uint32_t(vwap_num_ / vwap_den_) : 0; }
    double   vwap_dollars() const { return vwap_units() / 10000.0; }

    uint64_t trade_count()  const { return trade_count_; }
    uint64_t trade_shares() const { return vwap_den_; }
    uint64_t total_messages() const { return total_messages_; }

    // ---- message rate -----------------------------------------------------
    // Instantaneous rate: messages observed in the trailing 1 second of event
    // time (ITCH timestamps are ns since midnight).
    uint64_t rolling_msg_rate() const { return window_.size(); }
    // Average over the whole session, in messages/sec of event time.
    double avg_msg_rate() const {
        if (last_ts_ <= first_ts_) return 0.0;
        double secs = double(last_ts_ - first_ts_) / 1e9;
        return secs > 0 ? double(total_messages_) / secs : 0.0;
    }

    // ---- book-derived statistics (computed on demand) ---------------------
    static uint32_t spread(const OrderBook& b) { return b.spread(); }
    static uint32_t best_bid_depth(const OrderBook& b) { return b.best_bid_depth(); }
    static uint32_t best_ask_depth(const OrderBook& b) { return b.best_ask_depth(); }

    // Order-flow imbalance over the top `levels` price levels, in [-1, 1].
    static double imbalance(const OrderBook& b, uint32_t levels = 5) {
        uint64_t bid = b.bid_volume_top(levels);
        uint64_t ask = b.ask_volume_top(levels);
        uint64_t tot = bid + ask;
        if (tot == 0) return 0.0;
        return (double(bid) - double(ask)) / double(tot);
    }
    // Fixed-point imbalance scaled to [-10000, 10000] (hardware-style).
    static int32_t imbalance_fixed(const OrderBook& b, uint32_t levels = 5) {
        return int32_t(imbalance(b, levels) * 10000.0);
    }

private:
    uint64_t vwap_num_ = 0;      // sum(price * shares)
    uint64_t vwap_den_ = 0;      // sum(shares)
    uint64_t trade_count_ = 0;
    uint64_t total_messages_ = 0;
    uint64_t first_ts_ = 0;
    uint64_t last_ts_  = 0;
    std::deque<uint64_t> window_;   // event timestamps within the last 1s
    static constexpr uint64_t ONE_SEC_NS = 1'000'000'000ULL;
};

}  // namespace ob
