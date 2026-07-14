// Queue-aware fill simulation for synthetic (strategy) orders.
//
// The replicated market book is never mutated: synthetic orders live only in
// this shadow overlay, keyed off the same decoded ITCH stream the BookEngine
// consumes. Consequences (documented modeling choices):
//   * No market impact — our taker fills do not deplete displayed liquidity,
//     and our resting orders are invisible to the market.
//   * Trade ('P') messages are hidden/odd-lot liquidity: they drive bars and
//     stop monitoring upstream but do not drain displayed queues here.
//
// Queue model (price-time priority against the displayed book):
//   * On resting at price P: queue_ahead = displayed same-side shares at P
//     plus any of our own earlier orders resting there; watermark = highest
//     market order_ref seen so far (ITCH refs are monotonic, so ref <=
//     watermark <=> that order was ahead of us in time).
//   * Execute of ref r at our (side, P):
//       r <= watermark: it was ahead — drain queue_ahead by q; any excess
//         beyond queue_ahead would have crossed our position, so it fills us
//         (maker, at P).
//       r >  watermark: the market traded through orders behind us — under
//         price-time priority we would have filled first, so it fills us,
//         with the event's q shared FIFO across our orders at the level.
//   * Execute of ref r at a WORSE same-side price than one of our orders (a
//     bid below our bid / an ask above our ask): price priority means we
//     would have filled first — fills us (maker), FIFO-shared across our
//     price-superior orders. This is what fills the resting remainder of a
//     crossed limit sitting at a level with no market orders on it.
//   * Cancel/Delete/Replace-out of r at our (side, P) with r <= watermark
//     shrinks queue_ahead. Replace's new ref re-arrives behind us.
//   * Any reduction of one of OUR orders (cancel, or a traded-through fill)
//     shrinks the queue_ahead of our later orders at the same level, whose
//     snapshot counted it. Maker fills need no such fix-up: they are part of
//     the same executed q that later orders already drain from.
//
// Call on_market_message(m) BEFORE BookEngine::apply(m): resolution of which
// resting order a message touches needs the pre-apply ref table.
#pragma once

#include "../book/order_book.hpp"
#include "../parser/itch_messages.hpp"

#include <cstdint>
#include <vector>

namespace ob::trade {

// Synthetic order refs live above every real ITCH ref.
constexpr uint64_t OUR_REF_BIT = 1ull << 63;

struct FillRec {
    uint64_t ts = 0;        // timestamp of the causing event (ns since midnight)
    uint64_t our_ref = 0;
    char     side = 0;      // 'B'/'S'
    uint32_t price = 0;     // fill price, raw ITCH units
    uint32_t shares = 0;
    bool     maker = false; // true: filled while resting; false: taker sweep
};

struct OpenOrder {
    uint64_t ref = 0;
    char     side = 0;
    uint32_t price = 0;
    uint32_t remaining = 0;
    uint32_t original = 0;
    uint64_t queue_ahead = 0;  // displayed shares ahead of us at this level
    uint64_t watermark = 0;    // market refs <= this were ahead of us
    uint64_t seq = 0;          // our insertion order (FIFO within a level)
};

class FillEngine {
public:
    FillEngine(BookEngine& eng, uint16_t locate) : eng_(eng), locate_(locate) {}

    // Limit order: sweeps displayed liquidity it crosses (taker fills), rests
    // the remainder with a queue position. Returns the synthetic ref (find()
    // is null if it fully filled on entry).
    uint64_t submit_limit(char side, uint32_t price, uint32_t shares, uint64_t ts);

    // Full cancel; false if the ref is not open.
    bool cancel(uint64_t our_ref);

    // Feed every decoded market message BEFORE eng.apply(m).
    void on_market_message(const DecodedMessage& m);

    // Fills accumulated since the last call (submit sweeps + maker fills).
    std::vector<FillRec> take_fills() {
        std::vector<FillRec> out;
        out.swap(fills_);
        return out;
    }

    const OpenOrder* find(uint64_t our_ref) const;
    const std::vector<OpenOrder>& open_orders() const { return open_; }
    uint64_t watermark() const { return max_market_ref_; }

private:
    void fill(OpenOrder& o, uint32_t shares, uint32_t price, bool maker, uint64_t ts);
    void handle_exec(uint64_t r, char side, uint32_t price, uint32_t q, uint64_t ts);
    void handle_remove(uint64_t r, char side, uint32_t price, uint32_t x);
    // Our order `o` lost `amount` shares outside the maker path: fix up the
    // queue snapshots of our later orders at the same level.
    void reduce_later(const OpenOrder& o, uint32_t amount);
    void erase_filled();

    BookEngine& eng_;
    uint16_t locate_;
    uint64_t next_ref_ = OUR_REF_BIT | 1;
    uint64_t next_seq_ = 0;
    uint64_t max_market_ref_ = 0;
    std::vector<OpenOrder> open_;   // insertion-ordered (seq ascending)
    std::vector<FillRec> fills_;
};

}  // namespace ob::trade
