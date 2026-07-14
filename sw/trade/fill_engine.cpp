#include "fill_engine.hpp"

#include <algorithm>
#include <cassert>

namespace ob::trade {

namespace {
inline uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
}

uint64_t FillEngine::submit_limit(char side, uint32_t price, uint32_t shares,
                                  uint64_t ts) {
    uint64_t ref = next_ref_++;
    uint32_t remaining = shares;

    // 1. Marketable sweep: walk displayed opposite-side levels from the touch
    // to our limit. Virtual liquidity — the public book is not depleted.
    const OrderBook* b = eng_.book(locate_);
    if (b) {
        if (side == SIDE_BUY) {
            for (uint32_t p = b->best_ask(); remaining && p && p <= price; ++p) {
                uint32_t avail = b->ask_shares_at(p);
                if (!avail) continue;
                uint32_t f = umin(remaining, avail);
                fills_.push_back({ts, ref, side, p, f, false});
                remaining -= f;
            }
        } else {
            for (uint32_t p = b->best_bid(); remaining && p && p >= price; --p) {
                uint32_t avail = b->bid_shares_at(p);
                if (!avail) continue;
                uint32_t f = umin(remaining, avail);
                fills_.push_back({ts, ref, side, p, f, false});
                remaining -= f;
            }
        }
    }

    // 2. Rest the remainder behind everything displayed at our level plus our
    // own earlier orders there.
    if (remaining) {
        uint64_t qa = 0;
        if (b) qa = (side == SIDE_BUY) ? b->bid_shares_at(price)
                                       : b->ask_shares_at(price);
        for (const OpenOrder& o : open_)
            if (o.side == side && o.price == price) qa += o.remaining;
        open_.push_back({ref, side, price, remaining, shares, qa,
                         max_market_ref_, next_seq_++});
    }
    return ref;
}

bool FillEngine::cancel(uint64_t our_ref) {
    for (size_t i = 0; i < open_.size(); ++i) {
        if (open_[i].ref != our_ref) continue;
        OpenOrder o = open_[i];
        open_.erase(open_.begin() + i);
        reduce_later(o, o.remaining);
        return true;
    }
    return false;
}

const OpenOrder* FillEngine::find(uint64_t our_ref) const {
    for (const OpenOrder& o : open_)
        if (o.ref == our_ref) return &o;
    return nullptr;
}

void FillEngine::on_market_message(const DecodedMessage& m) {
    switch (m.type) {
        case MsgType::AddOrder:
        case MsgType::AddOrderMPID:
            if (m.order_ref > max_market_ref_) max_market_ref_ = m.order_ref;
            return;  // new arrivals rest behind us: no queue effect
        case MsgType::OrderExecuted:
        case MsgType::OrderExecutedPrice: {
            const auto* v = eng_.refs().find(m.order_ref);
            if (!v || v->locate != locate_) return;
            uint32_t q = umin(m.shares, v->shares);
            handle_exec(m.order_ref, v->side, v->price, q, m.timestamp);
            return;
        }
        case MsgType::OrderCancel: {
            const auto* v = eng_.refs().find(m.order_ref);
            if (!v || v->locate != locate_) return;
            handle_remove(m.order_ref, v->side, v->price, umin(m.shares, v->shares));
            return;
        }
        case MsgType::OrderDelete: {
            const auto* v = eng_.refs().find(m.order_ref);
            if (!v || v->locate != locate_) return;
            handle_remove(m.order_ref, v->side, v->price, v->shares);
            return;
        }
        case MsgType::OrderReplace: {
            const auto* v = eng_.refs().find(m.order_ref);
            if (v && v->locate == locate_)
                handle_remove(m.order_ref, v->side, v->price, v->shares);
            if (m.new_order_ref > max_market_ref_)
                max_market_ref_ = m.new_order_ref;  // re-arrives behind us
            return;
        }
        case MsgType::Trade:  // hidden liquidity: no displayed-queue effect
        default:
            return;
    }
}

void FillEngine::fill(OpenOrder& o, uint32_t shares, uint32_t price, bool maker,
                      uint64_t ts) {
    if (shares == 0) return;
    assert(shares <= o.remaining);
    fills_.push_back({ts, o.ref, o.side, price, shares, maker});
    o.remaining -= shares;
}

void FillEngine::handle_exec(uint64_t r, char side, uint32_t price, uint32_t q,
                             uint64_t ts) {
    // Price-through: aggressor flow consumed a same-side order at a worse
    // price than ours (a bid below our bid / an ask above our ask). Price
    // priority says we fill first, whatever our queue position. FIFO-shared
    // budget across our price-superior orders.
    uint32_t pthrough = q;
    for (OpenOrder& o : open_) {
        if (o.side != side || o.remaining == 0) continue;
        bool superior = (side == SIDE_BUY) ? o.price > price : o.price < price;
        if (!superior) continue;
        uint32_t f = umin(pthrough, o.remaining);
        fill(o, f, o.price, true, ts);
        pthrough -= f;
        if (f) reduce_later(o, f);
    }

    // `through`: FIFO-shared budget for the traded-through path (r behind us).
    uint32_t through = q;
    for (OpenOrder& o : open_) {
        if (o.side != side || o.price != price || o.remaining == 0) continue;
        if (r <= o.watermark) {
            // Executed order was ahead of us. Each of our orders drains the
            // full q independently: a later order's queue_ahead includes the
            // earlier orders' remaining, so maker fills stay consistent.
            uint64_t take = q < o.queue_ahead ? q : o.queue_ahead;
            o.queue_ahead -= take;
            uint32_t leftover = q - (uint32_t)take;
            fill(o, umin(leftover, o.remaining), o.price, true, ts);
        } else {
            // Market traded through orders behind us — we'd have filled first.
            uint32_t f = umin(through, o.remaining);
            fill(o, f, o.price, true, ts);
            through -= f;
            if (f) reduce_later(o, f);  // later snapshots counted these shares
        }
    }
    erase_filled();
}

void FillEngine::handle_remove(uint64_t r, char side, uint32_t price, uint32_t x) {
    for (OpenOrder& o : open_) {
        if (o.side != side || o.price != price) continue;
        if (r <= o.watermark)
            o.queue_ahead -= (x < o.queue_ahead) ? x : o.queue_ahead;
    }
}

void FillEngine::reduce_later(const OpenOrder& o, uint32_t amount) {
    for (OpenOrder& later : open_) {
        if (later.seq <= o.seq || later.side != o.side || later.price != o.price)
            continue;
        later.queue_ahead -= (amount < later.queue_ahead) ? amount
                                                          : later.queue_ahead;
    }
}

void FillEngine::erase_filled() {
    open_.erase(std::remove_if(open_.begin(), open_.end(),
                               [](const OpenOrder& o) { return o.remaining == 0; }),
                open_.end());
}

}  // namespace ob::trade
