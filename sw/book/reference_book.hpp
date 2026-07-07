// The correctness-first book: deliberately simple and obviously right, built
// on std::map (red-black tree) and std::unordered_map. It's the oracle — the
// SIMD/direct-indexed FastBook is validated by replaying the same ITCH stream
// through both and asserting identical state. Best bid is the largest key on
// the bid tree (rbegin); best ask the smallest on the ask tree (begin).
#pragma once

#include "../parser/itch_messages.hpp"

#include <cstdint>
#include <map>
#include <unordered_map>

namespace ob {

class ReferenceBook {
public:
    struct Order {
        uint16_t locate;
        char     side;
        uint32_t price;
        uint32_t shares;
    };

    struct Side {
        // price -> total shares, and price -> resting order count
        std::map<uint32_t, uint32_t> shares;
        std::map<uint32_t, uint32_t> orders;
        void add(uint32_t price, uint32_t qty) {
            shares[price] += qty;
            orders[price] += 1;
        }
        void remove(uint32_t price, uint32_t qty) {
            auto it = shares.find(price);
            if (it == shares.end()) return;
            it->second -= qty;
            auto ot = orders.find(price);
            if (ot != orders.end() && ot->second > 0) ot->second -= 1;
            if (it->second == 0) { shares.erase(it);
                                   if (ot != orders.end()) orders.erase(ot); }
        }
        // Reduce shares at a price WITHOUT removing an order (partial exec/cancel).
        void reduce(uint32_t price, uint32_t qty, bool order_gone) {
            auto it = shares.find(price);
            if (it == shares.end()) return;
            it->second -= qty;
            if (order_gone) {
                auto ot = orders.find(price);
                if (ot != orders.end() && ot->second > 0) ot->second -= 1;
            }
            if (it->second == 0) {
                shares.erase(it);
                auto ot = orders.find(price);
                if (ot != orders.end()) orders.erase(ot);
            }
        }
    };

    struct SymbolBook { Side bid, ask; };

    void apply(const DecodedMessage& m) {
        switch (m.type) {
            case MsgType::AddOrder:
            case MsgType::AddOrderMPID:        on_add(m);     break;
            case MsgType::OrderExecuted:       on_exec(m);    break;
            case MsgType::OrderExecutedPrice:  on_exec(m);    break;  // same book effect
            case MsgType::OrderCancel:         on_cancel(m);  break;
            case MsgType::OrderDelete:         on_delete(m);  break;
            case MsgType::OrderReplace:        on_replace(m); break;
            case MsgType::Trade:               break;  // no book effect
            case MsgType::SystemEvent:         break;
            default:                           break;
        }
    }

    // ---- queries -----------------------------------------------------------
    bool has_symbol(uint16_t locate) const { return books_.count(locate) != 0; }

    // 0 means "no bid/ask".
    uint32_t best_bid(uint16_t locate) const {
        auto it = books_.find(locate);
        if (it == books_.end() || it->second.bid.shares.empty()) return 0;
        return it->second.bid.shares.rbegin()->first;   // largest bid price
    }
    uint32_t best_ask(uint16_t locate) const {
        auto it = books_.find(locate);
        if (it == books_.end() || it->second.ask.shares.empty()) return 0;
        return it->second.ask.shares.begin()->first;    // smallest ask price
    }
    uint32_t bid_shares_at(uint16_t locate, uint32_t price) const {
        auto it = books_.find(locate);
        if (it == books_.end()) return 0;
        auto s = it->second.bid.shares.find(price);
        return s == it->second.bid.shares.end() ? 0 : s->second;
    }
    uint32_t ask_shares_at(uint16_t locate, uint32_t price) const {
        auto it = books_.find(locate);
        if (it == books_.end()) return 0;
        auto s = it->second.ask.shares.find(price);
        return s == it->second.ask.shares.end() ? 0 : s->second;
    }
    uint32_t best_bid_depth(uint16_t locate) const {
        uint32_t p = best_bid(locate); return p ? bid_shares_at(locate, p) : 0;
    }
    uint32_t best_ask_depth(uint16_t locate) const {
        uint32_t p = best_ask(locate); return p ? ask_shares_at(locate, p) : 0;
    }
    size_t live_orders() const { return orders_.size(); }
    const std::unordered_map<uint16_t, SymbolBook>& books() const { return books_; }

private:
    Side& side_ref(SymbolBook& b, char side) { return side == SIDE_BUY ? b.bid : b.ask; }

    void on_add(const DecodedMessage& m) {
        SymbolBook& b = books_[m.stock_locate];
        side_ref(b, m.side).add(m.price, m.shares);
        orders_[m.order_ref] = Order{m.stock_locate, m.side, m.price, m.shares};
    }
    void on_exec(const DecodedMessage& m) {
        auto it = orders_.find(m.order_ref);
        if (it == orders_.end()) return;
        Order& o = it->second;
        uint32_t ex = m.shares > o.shares ? o.shares : m.shares;
        SymbolBook& b = books_[o.locate];
        o.shares -= ex;
        const bool gone = (o.shares == 0);
        side_ref(b, o.side).reduce(o.price, ex, gone);
        if (gone) orders_.erase(it);
    }
    void on_cancel(const DecodedMessage& m) {
        auto it = orders_.find(m.order_ref);
        if (it == orders_.end()) return;
        Order& o = it->second;
        uint32_t cx = m.shares > o.shares ? o.shares : m.shares;
        SymbolBook& b = books_[o.locate];
        o.shares -= cx;
        const bool gone = (o.shares == 0);
        side_ref(b, o.side).reduce(o.price, cx, gone);
        if (gone) orders_.erase(it);
    }
    void on_delete(const DecodedMessage& m) {
        auto it = orders_.find(m.order_ref);
        if (it == orders_.end()) return;
        Order o = it->second;
        SymbolBook& b = books_[o.locate];
        side_ref(b, o.side).reduce(o.price, o.shares, true);
        orders_.erase(it);
    }
    void on_replace(const DecodedMessage& m) {
        auto it = orders_.find(m.order_ref);   // original ref
        if (it == orders_.end()) return;
        Order o = it->second;                  // capture old side/locate/price/shares
        SymbolBook& b = books_[o.locate];
        side_ref(b, o.side).reduce(o.price, o.shares, true);   // delete old
        orders_.erase(it);
        // add new order, inheriting side & locate
        side_ref(b, o.side).add(m.price, m.shares);
        orders_[m.new_order_ref] = Order{o.locate, o.side, m.price, m.shares};
    }

    std::unordered_map<uint16_t, SymbolBook>     books_;
    std::unordered_map<uint64_t, Order>          orders_;
};

}  // namespace ob
