// Self-contained golden order book (std::map based) — the reference the RTL is
// diffed against. Deliberately independent of the sw/ implementation: it parses
// the ITCH bytes at the fixed field offsets (same layout as itch_messages.hpp /
// rtl/ob_pkg.sv) and applies the book semantics with plain std::map /
// std::unordered_map. Correctness over performance.
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>

namespace obsim {

// Normalized message (mirrors rtl decoded_t).
struct RefMsg {
    uint8_t  type = 0;          // ASCII type code
    bool     is_bid = false;
    bool     printable = false;
    uint16_t locate = 0;
    uint32_t price = 0;
    uint32_t shares = 0;
    uint64_t order_ref = 0;
    uint64_t new_order_ref = 0;
};

// Parse one message body into a normalized RefMsg.
RefMsg decode_body(const std::vector<uint8_t>& body);

struct Level { uint64_t shares = 0; uint32_t count = 0; };

class RefBook {
public:
    void apply(const RefMsg& m);

    // --- book queries (match the RTL outputs) ---
    bool     best_bid_valid() const { return !bids_.empty(); }
    uint32_t best_bid_price() const { return bids_.empty() ? 0 : bids_.rbegin()->first; }
    uint64_t best_bid_shares() const { return bids_.empty() ? 0 : bids_.rbegin()->second.shares; }
    bool     best_ask_valid() const { return !asks_.empty(); }
    uint32_t best_ask_price() const { return asks_.empty() ? 0 : asks_.begin()->first; }
    uint64_t best_ask_shares() const { return asks_.empty() ? 0 : asks_.begin()->second.shares; }

    uint64_t level_shares(bool is_bid, uint32_t price) const {
        const auto& m = is_bid ? bids_ : asks_;
        auto it = m.find(price);
        return it == m.end() ? 0 : it->second.shares;
    }
    uint32_t level_count(bool is_bid, uint32_t price) const {
        const auto& m = is_bid ? bids_ : asks_;
        auto it = m.find(price);
        return it == m.end() ? 0 : it->second.count;
    }

    uint64_t tot_bid_vol() const { return tot_bid_; }
    uint64_t tot_ask_vol() const { return tot_ask_; }

    // --- statistics ---
    uint64_t vwap_num() const { return vwap_num_; }
    uint64_t vwap_den() const { return vwap_den_; }
    uint64_t trade_count() const { return trade_count_; }
    bool     spread_valid() const { return best_bid_valid() && best_ask_valid(); }
    uint32_t spread() const {
        return spread_valid() ? (best_ask_price() - best_bid_price()) : 0;
    }

    // active price levels (for level-sweep diffs)
    std::vector<uint32_t> bid_prices() const {
        std::vector<uint32_t> v; for (auto& kv : bids_) v.push_back(kv.first); return v;
    }
    std::vector<uint32_t> ask_prices() const {
        std::vector<uint32_t> v; for (auto& kv : asks_) v.push_back(kv.first); return v;
    }

private:
    struct Order { bool is_bid; uint16_t locate; uint32_t price; uint64_t shares; };

    void add_order(uint64_t ref, bool is_bid, uint16_t locate, uint32_t price, uint64_t shares);
    void reduce_order(uint64_t ref, uint64_t amt);   // partial; removes if hits 0
    void remove_order(uint64_t ref);                 // full delete

    std::map<uint32_t, Level>            bids_;
    std::map<uint32_t, Level>            asks_;
    std::unordered_map<uint64_t, Order>  orders_;
    uint64_t tot_bid_ = 0, tot_ask_ = 0;
    uint64_t vwap_num_ = 0, vwap_den_ = 0, trade_count_ = 0;
};

}  // namespace obsim
