// The fast order book.
//
// Each side is a price-indexed flat array (PriceLevel[price]), so an update is
// O(1): index by price, add/subtract shares. Best bid/ask are maintained
// incrementally; only when a Delete/Execute empties the current best level do
// we scan for the next non-empty level, and that scan is vectorised (AVX-512 /
// AVX2 gather of 8 levels per step, scalar fallback). Direct indexing is what
// replaces the reference book's std::map tree traversal.
//
// A BookEngine ties together the order-reference table and one OrderBook per
// stock_locate, and applies decoded ITCH messages with the exact same semantics
// as ReferenceBook — the two are diffed in the test suite.
#pragma once

#include "order_ref_table.hpp"
#include "../parser/itch_messages.hpp"
#include "../util/simd.hpp"

#include <cstdint>
#include <vector>
#include <memory>
#include <unordered_map>

namespace ob {

struct PriceLevel {
    uint32_t total_shares = 0;
    uint32_t order_count  = 0;
};

class OrderBook {
public:
    explicit OrderBook(uint32_t price_levels = 1'000'000)
        : max_price_(price_levels - 1),
          bid_(price_levels), ask_(price_levels) {}

    uint32_t best_bid() const { return best_bid_; }
    uint32_t best_ask() const { return best_ask_; }
    uint32_t best_bid_depth() const { return best_bid_ ? bid_[best_bid_].total_shares : 0; }
    uint32_t best_ask_depth() const { return best_ask_ ? ask_[best_ask_].total_shares : 0; }
    uint32_t bid_shares_at(uint32_t p) const { return p <= max_price_ ? bid_[p].total_shares : 0; }
    uint32_t ask_shares_at(uint32_t p) const { return p <= max_price_ ? ask_[p].total_shares : 0; }

    // spread in ticks-of-1/10000; 0 if either side empty.
    uint32_t spread() const {
        return (best_bid_ && best_ask_ && best_ask_ > best_bid_) ? best_ask_ - best_bid_ : 0;
    }

    uint64_t scan_count() const { return scan_count_; }
    uint64_t scan_steps() const { return scan_steps_; }

    void add(char side, uint32_t price, uint32_t shares) {
        if (price > max_price_) return;
        if (side == SIDE_BUY) {
            bid_[price].total_shares += shares;
            bid_[price].order_count  += 1;
            if (price > best_bid_) best_bid_ = price;
        } else {
            ask_[price].total_shares += shares;
            ask_[price].order_count  += 1;
            if (best_ask_ == 0 || price < best_ask_) best_ask_ = price;
        }
    }

    // Subtract shares; if order_gone, also decrement order_count. Rescans for a
    // new best only when the current best level is emptied.
    void remove(char side, uint32_t price, uint32_t shares, bool order_gone) {
        if (price > max_price_) return;
        if (side == SIDE_BUY) {
            PriceLevel& L = bid_[price];
            L.total_shares -= (shares <= L.total_shares ? shares : L.total_shares);
            if (order_gone && L.order_count) L.order_count -= 1;
            if (L.total_shares == 0 && price == best_bid_)
                best_bid_ = scan_down(price == 0 ? 0 : price - 1);
        } else {
            PriceLevel& L = ask_[price];
            L.total_shares -= (shares <= L.total_shares ? shares : L.total_shares);
            if (order_gone && L.order_count) L.order_count -= 1;
            if (L.total_shares == 0 && price == best_ask_)
                best_ask_ = scan_up(price + 1);
        }
    }

    // Sum of shares across the top `n` non-empty bid levels (for imbalance).
    uint64_t bid_volume_top(uint32_t n) const { return vol_top(bid_, best_bid_, n, true); }
    uint64_t ask_volume_top(uint32_t n) const { return vol_top(ask_, best_ask_, n, false); }

private:
    // Highest price <= start with shares>0, else 0.
    uint32_t scan_down(uint32_t start) {
        ++scan_count_;
        if (start == 0 || start > max_price_) start = (start > max_price_) ? max_price_ : 0;
        int64_t p = int64_t(start);
#if defined(OB_SIMD_AVX2) || defined(OB_SIMD_AVX512)
        const int* base = reinterpret_cast<const int*>(&bid_[0].total_shares);
        const __m256i zero = _mm256_setzero_si256();
        while (p >= 8) {
            __m256i idx = _mm256_set_epi32(int(p-7),int(p-6),int(p-5),int(p-4),
                                           int(p-3),int(p-2),int(p-1),int(p));
            __m256i v   = _mm256_i32gather_epi32(base, idx, sizeof(PriceLevel));
            unsigned m  = unsigned(_mm256_movemask_ps(
                _mm256_castsi256_ps(_mm256_cmpgt_epi32(v, zero))));
            scan_steps_ += 8;
            if (m) return uint32_t(p) - unsigned(__builtin_ctz(m));
            p -= 8;
        }
#endif
        for (; p >= 1; --p) { ++scan_steps_; if (bid_[p].total_shares) return uint32_t(p); }
        return 0;
    }

    // Lowest price >= start with shares>0, else 0.
    uint32_t scan_up(uint32_t start) {
        ++scan_count_;
        if (start == 0) start = 1;
        int64_t p = int64_t(start);
        const int64_t hi = int64_t(max_price_);
#if defined(OB_SIMD_AVX2) || defined(OB_SIMD_AVX512)
        const int* base = reinterpret_cast<const int*>(&ask_[0].total_shares);
        const __m256i zero = _mm256_setzero_si256();
        while (p + 8 <= hi + 1) {
            __m256i idx = _mm256_set_epi32(int(p+7),int(p+6),int(p+5),int(p+4),
                                           int(p+3),int(p+2),int(p+1),int(p));
            __m256i v   = _mm256_i32gather_epi32(base, idx, sizeof(PriceLevel));
            unsigned m  = unsigned(_mm256_movemask_ps(
                _mm256_castsi256_ps(_mm256_cmpgt_epi32(v, zero))));
            scan_steps_ += 8;
            if (m) return uint32_t(p) + unsigned(__builtin_ctz(m));
            p += 8;
        }
#endif
        for (; p <= hi; ++p) { ++scan_steps_; if (ask_[p].total_shares) return uint32_t(p); }
        return 0;
    }

    static uint64_t vol_top(const std::vector<PriceLevel>& side, uint32_t best,
                            uint32_t n, bool descending) {
        if (best == 0 || n == 0) return 0;
        uint64_t sum = 0; uint32_t seen = 0;
        if (descending) {
            for (int64_t p = best; p >= 1 && seen < n; --p)
                if (side[p].total_shares) { sum += side[p].total_shares; ++seen; }
        } else {
            for (size_t p = best; p < side.size() && seen < n; ++p)
                if (side[p].total_shares) { sum += side[p].total_shares; ++seen; }
        }
        return sum;
    }

    uint32_t max_price_;
    std::vector<PriceLevel> bid_, ask_;
    uint32_t best_bid_ = 0, best_ask_ = 0;
    uint64_t scan_count_ = 0, scan_steps_ = 0;
};

// ---------------------------------------------------------------------------
// BookEngine: order-ref table + per-symbol OrderBook, applies decoded ITCH.
// ---------------------------------------------------------------------------
class StatsEngine;  // fwd

class BookEngine {
public:
    explicit BookEngine(uint32_t price_levels = 1'000'000,
                        uint32_t ref_capacity = (1u << 20))
        : price_levels_(price_levels), refs_(ref_capacity) {}

    OrderRefTable&       refs()       { return refs_; }
    const OrderRefTable& refs() const { return refs_; }

    OrderBook* book(uint16_t locate) {
        auto it = books_.find(locate);
        return it == books_.end() ? nullptr : it->second.get();
    }
    const OrderBook* book(uint16_t locate) const {
        auto it = books_.find(locate);
        return it == books_.end() ? nullptr : it->second.get();
    }

    // Counters used by perf reporting.
    struct Counters {
        uint64_t add = 0, exec = 0, cancel = 0, del = 0, replace = 0, trade = 0, sys = 0;
    };
    const Counters& counters() const { return ctr_; }

    // Execution info surfaced for the stats engine (VWAP feed). `contributes`
    // is true for messages that represent an actual trade print (E, C-printable,
    // P) — the price is the resolved trade price (resting price for E, execution
    // price for C, trade price for P).
    struct ExecResult {
        bool     contributes = false;
        uint32_t price       = 0;
        uint32_t shares      = 0;
        uint16_t locate      = 0;
    };

    // Apply a decoded message. Returns the affected book (or nullptr). If `out`
    // is non-null it is filled with trade/VWAP info for this message.
    OrderBook* apply(const DecodedMessage& m, ExecResult* out = nullptr) {
        if (out) *out = ExecResult{};
        switch (m.type) {
            case MsgType::AddOrder:
            case MsgType::AddOrderMPID:       return on_add(m);
            case MsgType::OrderExecuted:
            case MsgType::OrderExecutedPrice: return on_reduce(m, true, out);  // exec
            case MsgType::OrderCancel:        return on_reduce(m, false, out); // cancel
            case MsgType::OrderDelete:        return on_delete(m);
            case MsgType::OrderReplace:       return on_replace(m);
            case MsgType::Trade:
                ++ctr_.trade;
                if (out) *out = ExecResult{true, m.price, m.shares, m.stock_locate};
                return nullptr;
            case MsgType::SystemEvent:        ++ctr_.sys;   return nullptr;
            default:                          return nullptr;
        }
    }

private:
    OrderBook& get_or_make(uint16_t locate) {
        auto it = books_.find(locate);
        if (it != books_.end()) return *it->second;
        auto ob = std::make_unique<OrderBook>(price_levels_);
        OrderBook* p = ob.get();
        books_.emplace(locate, std::move(ob));
        return *p;
    }

    OrderBook* on_add(const DecodedMessage& m) {
        ++ctr_.add;
        OrderBook& b = get_or_make(m.stock_locate);
        b.add(m.side, m.price, m.shares);
        refs_.insert(m.order_ref,
                     OrderRefTable::Value{m.price, m.shares, m.stock_locate, m.side, 0});
        return &b;
    }

    OrderBook* on_reduce(const DecodedMessage& m, bool is_exec, ExecResult* out) {
        if (is_exec) ++ctr_.exec; else ++ctr_.cancel;
        auto* v = refs_.find(m.order_ref);
        if (!v) return nullptr;
        uint32_t qty = m.shares > v->shares ? v->shares : m.shares;
        OrderBook& b = get_or_make(v->locate);
        if (out && is_exec) {
            // E trades at the resting price; C ('ExecutedPrice') at its own
            // execution price and only if printable.
            const bool printable = (m.type == MsgType::OrderExecutedPrice)
                                       ? (m.printable != 0) : true;
            const uint32_t px = (m.type == MsgType::OrderExecutedPrice)
                                    ? m.price : v->price;
            *out = ExecResult{printable, px, qty, v->locate};
        }
        v->shares -= qty;
        const bool gone = (v->shares == 0);
        b.remove(v->side, v->price, qty, gone);
        if (gone) refs_.erase(m.order_ref);
        return &b;
    }

    OrderBook* on_delete(const DecodedMessage& m) {
        ++ctr_.del;
        auto* v = refs_.find(m.order_ref);
        if (!v) return nullptr;
        OrderRefTable::Value val = *v;       // copy before erase
        OrderBook& b = get_or_make(val.locate);
        b.remove(val.side, val.price, val.shares, true);
        refs_.erase(m.order_ref);
        return &b;
    }

    OrderBook* on_replace(const DecodedMessage& m) {
        ++ctr_.replace;
        auto* v = refs_.find(m.order_ref);   // original
        if (!v) return nullptr;
        OrderRefTable::Value old = *v;
        OrderBook& b = get_or_make(old.locate);
        b.remove(old.side, old.price, old.shares, true);     // cancel old
        refs_.erase(m.order_ref);
        b.add(old.side, m.price, m.shares);                  // add new
        refs_.insert(m.new_order_ref,
                     OrderRefTable::Value{m.price, m.shares, old.locate, old.side, 0});
        return &b;
    }

    uint32_t price_levels_;
    OrderRefTable refs_;
    std::unordered_map<uint16_t, std::unique_ptr<OrderBook>> books_;
    Counters ctr_;
};

}  // namespace ob
