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
#include <limits>
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
        : max_price_(price_levels > 0 ? price_levels - 1 : 0),
          bid_(price_levels > 0 ? price_levels : 1),
          ask_(price_levels > 0 ? price_levels : 1) {}

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

    bool can_add(char side, uint32_t price, uint32_t shares) const {
        if ((side != SIDE_BUY && side != SIDE_SELL) || price == 0 ||
            price > max_price_ || shares == 0)
            return false;
        const PriceLevel& level = side == SIDE_BUY ? bid_[price] : ask_[price];
        return (level.total_shares == 0) == (level.order_count == 0) &&
               level.total_shares <= std::numeric_limits<uint32_t>::max() - shares &&
               level.order_count != std::numeric_limits<uint32_t>::max();
    }

    bool add(char side, uint32_t price, uint32_t shares) {
        if (!can_add(side, price, shares)) return false;
        if (side == SIDE_BUY) {
            bid_[price].total_shares += shares;
            bid_[price].order_count  += 1;
            if (price > best_bid_) best_bid_ = price;
        } else {
            ask_[price].total_shares += shares;
            ask_[price].order_count  += 1;
            if (best_ask_ == 0 || price < best_ask_) best_ask_ = price;
        }
        return true;
    }

    // Subtract shares; if order_gone, also decrement order_count. Rescans for a
    // new best only when the current best level is emptied.
    bool can_remove(char side, uint32_t price, uint32_t shares,
                    bool order_gone) const {
        if ((side != SIDE_BUY && side != SIDE_SELL) || price == 0 ||
            price > max_price_ || shares == 0)
            return false;
        const PriceLevel& level = side == SIDE_BUY ? bid_[price] : ask_[price];
        if (shares > level.total_shares || (order_gone && level.order_count == 0))
            return false;
        const uint32_t remaining_shares = level.total_shares - shares;
        const uint32_t remaining_orders = level.order_count - (order_gone ? 1u : 0u);
        return (remaining_shares == 0) == (remaining_orders == 0);
    }

    bool remove(char side, uint32_t price, uint32_t shares, bool order_gone) {
        if (!can_remove(side, price, shares, order_gone)) return false;
        if (side == SIDE_BUY) {
            PriceLevel& L = bid_[price];
            L.total_shares -= shares;
            if (order_gone) L.order_count -= 1;
            if (L.total_shares == 0 && price == best_bid_)
                best_bid_ = scan_down(price == 0 ? 0 : price - 1);
        } else {
            PriceLevel& L = ask_[price];
            L.total_shares -= shares;
            if (order_gone) L.order_count -= 1;
            if (L.total_shares == 0 && price == best_ask_)
                best_ask_ = scan_up(price + 1);
        }
        return true;
    }

    bool can_replace(char side, uint32_t old_price, uint32_t old_shares,
                     uint32_t new_price, uint32_t new_shares) const {
        if (!can_remove(side, old_price, old_shares, true) || new_price == 0 ||
            new_price > max_price_ || new_shares == 0)
            return false;
        if (old_price != new_price) return can_add(side, new_price, new_shares);

        const PriceLevel& level = side == SIDE_BUY ? bid_[old_price] : ask_[old_price];
        const uint32_t after_remove = level.total_shares - old_shares;
        return after_remove <= std::numeric_limits<uint32_t>::max() - new_shares;
    }

    bool replace(char side, uint32_t old_price, uint32_t old_shares,
                 uint32_t new_price, uint32_t new_shares) {
        if (!can_replace(side, old_price, old_shares, new_price, new_shares))
            return false;
        const bool removed = remove(side, old_price, old_shares, true);
        const bool added = add(side, new_price, new_shares);
        return removed && added;
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
    enum class ApplyStatus : uint8_t {
        Ignored,
        Applied,
        InvalidEvent,
        DuplicateReference,
        MissingReference,
        ReferenceTableFull,
        ResyncRequired,
    };

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
        uint64_t invalid = 0, duplicate_ref = 0, missing_ref = 0;
        uint64_t ref_insert_fail = 0, resync_required = 0;
    };
    const Counters& counters() const { return ctr_; }
    ApplyStatus last_status() const { return last_status_; }

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
        last_status_ = ApplyStatus::Ignored;
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
                last_status_ = ApplyStatus::Applied;
                if (out) *out = ExecResult{true, m.price, m.shares, m.stock_locate};
                return nullptr;
            case MsgType::SystemEvent:
                ++ctr_.sys;
                last_status_ = ApplyStatus::Applied;
                return nullptr;
            default:                          return nullptr;
        }
    }

private:
    OrderBook* reject(ApplyStatus status) {
        last_status_ = status;
        switch (status) {
            case ApplyStatus::InvalidEvent:       ++ctr_.invalid; break;
            case ApplyStatus::DuplicateReference: ++ctr_.duplicate_ref; break;
            case ApplyStatus::MissingReference:   ++ctr_.missing_ref; break;
            case ApplyStatus::ReferenceTableFull: ++ctr_.ref_insert_fail; break;
            case ApplyStatus::ResyncRequired:     ++ctr_.resync_required; break;
            default: break;
        }
        return nullptr;
    }

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
        if ((m.side != SIDE_BUY && m.side != SIDE_SELL) || m.order_ref == 0 ||
            m.stock_locate == 0 || m.price == 0 || m.price >= price_levels_ ||
            m.shares == 0)
            return reject(ApplyStatus::InvalidEvent);
        if (refs_.find(m.order_ref))
            return reject(ApplyStatus::DuplicateReference);
        OrderBook& b = get_or_make(m.stock_locate);
        if (!b.can_add(m.side, m.price, m.shares))
            return reject(ApplyStatus::InvalidEvent);
        if (!refs_.insert(m.order_ref,
                          OrderRefTable::Value{m.price, m.shares, m.stock_locate, m.side, 0}))
            return reject(ApplyStatus::ReferenceTableFull);
        if (!b.add(m.side, m.price, m.shares)) {
            refs_.erase(m.order_ref);
            return reject(ApplyStatus::ResyncRequired);
        }
        last_status_ = ApplyStatus::Applied;
        return &b;
    }

    OrderBook* on_reduce(const DecodedMessage& m, bool is_exec, ExecResult* out) {
        if (is_exec) ++ctr_.exec; else ++ctr_.cancel;
        if (m.order_ref == 0 || m.shares == 0)
            return reject(ApplyStatus::InvalidEvent);
        auto* v = refs_.find(m.order_ref);
        if (!v) return reject(ApplyStatus::MissingReference);
        if (m.shares > v->shares)
            return reject(ApplyStatus::InvalidEvent);
        const uint32_t qty = m.shares;
        OrderBook& b = get_or_make(v->locate);
        // Capture fields before erase, but publish the execution only after
        // the book update succeeds atomically.
        const bool printable = (m.type == MsgType::OrderExecutedPrice)
                                   ? (m.printable != 0) : true;
        const uint32_t exec_px = (m.type == MsgType::OrderExecutedPrice)
                                     ? m.price : v->price;
        const uint16_t locate = v->locate;
        const bool will_be_gone = (v->shares == qty);
        if (!b.remove(v->side, v->price, qty, will_be_gone))
            return reject(ApplyStatus::ResyncRequired);
        v->shares -= qty;
        if (will_be_gone) refs_.erase(m.order_ref);
        if (out && is_exec) *out = ExecResult{printable, exec_px, qty, locate};
        last_status_ = ApplyStatus::Applied;
        return &b;
    }

    OrderBook* on_delete(const DecodedMessage& m) {
        ++ctr_.del;
        auto* v = refs_.find(m.order_ref);
        if (!v) return reject(ApplyStatus::MissingReference);
        OrderRefTable::Value val = *v;       // copy before erase
        OrderBook& b = get_or_make(val.locate);
        if (!b.remove(val.side, val.price, val.shares, true))
            return reject(ApplyStatus::ResyncRequired);
        refs_.erase(m.order_ref);
        last_status_ = ApplyStatus::Applied;
        return &b;
    }

    OrderBook* on_replace(const DecodedMessage& m) {
        ++ctr_.replace;
        auto* v = refs_.find(m.order_ref);   // original
        if (!v) return reject(ApplyStatus::MissingReference);
        OrderRefTable::Value old = *v;
        if (m.new_order_ref == 0 || m.new_order_ref == m.order_ref ||
            m.price == 0 || m.price >= price_levels_ || m.shares == 0)
            return reject(ApplyStatus::InvalidEvent);
        if (refs_.find(m.new_order_ref))
            return reject(ApplyStatus::DuplicateReference);
        OrderBook& b = get_or_make(old.locate);
        if (!b.can_replace(old.side, old.price, old.shares, m.price, m.shares))
            return reject(ApplyStatus::InvalidEvent);

        // Replacing a key never increases table occupancy. Erasing first makes
        // one slot available; rollback preserves the old order if insertion
        // unexpectedly fails.
        refs_.erase(m.order_ref);
        if (!refs_.insert(m.new_order_ref,
                          OrderRefTable::Value{m.price, m.shares, old.locate, old.side, 0})) {
            refs_.insert(m.order_ref, old);
            return reject(ApplyStatus::ReferenceTableFull);
        }
        if (!b.replace(old.side, old.price, old.shares, m.price, m.shares)) {
            refs_.erase(m.new_order_ref);
            refs_.insert(m.order_ref, old);
            return reject(ApplyStatus::ResyncRequired);
        }
        last_status_ = ApplyStatus::Applied;
        return &b;
    }

    uint32_t price_levels_;
    OrderRefTable refs_;
    std::unordered_map<uint16_t, std::unique_ptr<OrderBook>> books_;
    Counters ctr_;
    ApplyStatus last_status_ = ApplyStatus::Ignored;
};

}  // namespace ob
