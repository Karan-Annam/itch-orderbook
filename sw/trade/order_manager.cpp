#include "order_manager.hpp"

#include <cmath>

namespace ob::trade {

const char* reason_name(int r) {
    switch (r) {
        case RSN_SIGNAL: return "signal";
        case RSN_STOP:   return "stop";
        case RSN_TRAIL:  return "trail";
        case RSN_TP:     return "tp";
        case RSN_EOD:    return "eod";
        default:         return "?";
    }
}

void OrderManager::poll_fills() {
    for (const FillRec& f : fe_.take_fills()) apply_fill(f);
}

void OrderManager::apply_fill(const FillRec& f) {
    fills_log_.push_back(f);
    float notional = (float)f.price * (float)f.shares;
    float fee = notional * cfg_.fee;
    cash_ -= fee;
    fees_open_ += fee;

    if (f.our_ref == entry_ref_) {
        if (side_ == 0) {           // first entry fill opens the position
            side_ = entry_dir_;
            entry_bar_ = cur_bar_;
            anchor_ = (float)f.price;
        }
        qty_ += f.shares;
        entry_shares_ += f.shares;
        entry_notional_ += notional;
        cash_ -= (float)side_ * notional;
        if (fe_.find(entry_ref_) == nullptr) entry_ref_ = 0;  // fully filled
    } else if (f.our_ref == exit_ref_) {
        uint32_t sh = f.shares <= qty_ ? f.shares : qty_;
        qty_ -= sh;
        exit_shares_ += sh;
        exit_notional_ += (float)f.price * (float)sh;
        cash_ += (float)side_ * (float)f.price * (float)sh;
        if (qty_ == 0) {
            if (fe_.find(exit_ref_)) fe_.cancel(exit_ref_);
            exit_ref_ = 0;
            close_round_trip(exit_reason_);
        } else if (fe_.find(exit_ref_) == nullptr) {
            exit_ref_ = 0;          // exit order exhausted with qty left over
        }
    }
}

void OrderManager::close_round_trip(int reason) {
    TradeRecT tr;
    tr.side = side_;
    tr.entry_t = entry_bar_;
    tr.exit_t = cur_bar_;
    tr.qty = (float)entry_shares_;
    tr.entry_px = entry_shares_ ? entry_notional_ / (float)entry_shares_ : 0.0f;
    tr.exit_px = exit_shares_ ? exit_notional_ / (float)exit_shares_ : 0.0f;
    // exit_shares_ == entry_shares_ here: exits are sized to the position
    tr.pnl = (float)side_ * (exit_notional_ - entry_notional_) - fees_open_;
    tr.reason = reason;
    trades_.push_back(tr);

    side_ = 0;
    qty_ = 0;
    entry_notional_ = exit_notional_ = 0.0f;
    entry_shares_ = exit_shares_ = 0;
    fees_open_ = 0.0f;
    entry_bar_ = -1;
    anchor_ = 0.0f;
}

void OrderManager::on_trade_print(uint32_t price, uint64_t ts) {
    last_print_ = price;
    if (side_ == 0 || exit_ref_ != 0) return;  // flat, or already exiting
    float px = (float)price;
    float entry_vwap = entry_shares_ ? entry_notional_ / (float)entry_shares_ : 0.0f;

    if (side_ > 0) {
        if (px > anchor_) anchor_ = px;
        if (cfg_stop_ > 0.0f && px <= entry_vwap * (1.0f - cfg_stop_)) {
            cancel_working_entry();
            submit_exit(RSN_STOP, ts);
        } else if (cfg_trail_ > 0.0f && px <= anchor_ * (1.0f - cfg_trail_)) {
            cancel_working_entry();
            submit_exit(RSN_TRAIL, ts);
        } else if (cfg_tp_ > 0.0f && px >= entry_vwap * (1.0f + cfg_tp_)) {
            cancel_working_entry();
            submit_exit(RSN_TP, ts);
        }
    } else {
        if (px < anchor_) anchor_ = px;
        if (cfg_stop_ > 0.0f && px >= entry_vwap * (1.0f + cfg_stop_)) {
            cancel_working_entry();
            submit_exit(RSN_STOP, ts);
        } else if (cfg_trail_ > 0.0f && px >= anchor_ * (1.0f + cfg_trail_)) {
            cancel_working_entry();
            submit_exit(RSN_TRAIL, ts);
        } else if (cfg_tp_ > 0.0f && px <= entry_vwap * (1.0f - cfg_tp_)) {
            cancel_working_entry();
            submit_exit(RSN_TP, ts);
        }
    }
}

VmCtx OrderManager::ctx(const BarSeries& bars, int t) const {
    VmCtx c;
    c.position = (float)side_;
    c.entry_px = (side_ != 0 && entry_shares_)
                     ? entry_notional_ / (float)entry_shares_ : 0.0f;
    c.equity = equity(bars.c[t]);
    return c;
}

void OrderManager::on_bar(const BarSeries& bars, int t, const Signals& sig,
                          uint64_t ts) {
    // Latch protective-exit configs for the coming bar (vm_core semantics:
    // this bar's program governs until the next bar's program runs).
    cfg_stop_ = sig.stop;
    cfg_tp_ = sig.tp;
    cfg_trail_ = sig.trail;

    // Arbitration, mirroring vm_core.h's signal block.
    if (side_ > 0 && sig.xl) {
        cancel_working_entry();
        if (exit_ref_ == 0) submit_exit(RSN_SIGNAL, ts);
    } else if (side_ < 0 && sig.xs) {
        cancel_working_entry();
        if (exit_ref_ == 0) submit_exit(RSN_SIGNAL, ts);
    } else if (side_ == 0 && exit_ref_ == 0) {
        if (sig.el && !sig.es) {
            if (entry_dir_ < 0) cancel_working_entry();  // flip: resubmit
            if (entry_ref_ == 0) { entry_dir_ = 1; submit_entry(1, sig.size, ts); }
        } else if (sig.es && !sig.el) {
            if (entry_dir_ > 0) cancel_working_entry();
            if (entry_ref_ == 0) { entry_dir_ = -1; submit_entry(-1, sig.size, ts); }
        }
    }

    // Re-peg a passive entry that is no longer at the touch (bar boundaries
    // only, so queue behavior stays deterministic and testable).
    if (entry_ref_ != 0 && !cfg_.entry_cross) {
        const OpenOrder* o = fe_.find(entry_ref_);
        if (o) {
            uint32_t best = entry_price(entry_dir_);
            if (best != 0 && best != o->price) {
                uint32_t remaining = o->remaining;
                fe_.cancel(entry_ref_);
                entry_ref_ = fe_.submit_limit(entry_dir_ > 0 ? SIDE_BUY : SIDE_SELL,
                                              best, remaining, ts);
                poll_fills();   // repegged order may cross and fill at once
                if (fe_.find(entry_ref_) == nullptr) entry_ref_ = 0;
            }
        } else {
            entry_ref_ = 0;
        }
    }

    (void)bars;
    cur_bar_ = t + 1;   // fills from here on belong to the forming bar
}

uint32_t OrderManager::entry_price(int dir) const {
    const OrderBook* b = eng_.book(locate_);
    if (!b) return 0;
    if (cfg_.entry_cross) {
        // marketable: through the opposite best
        if (dir > 0) return b->best_ask() ? b->best_ask() + cfg_.cross_ticks : 0;
        return b->best_bid() > cfg_.cross_ticks ? b->best_bid() - cfg_.cross_ticks : 0;
    }
    // passive: join our side's best
    return dir > 0 ? b->best_bid() : b->best_ask();
}

uint32_t OrderManager::exit_price() const {
    const OrderBook* b = eng_.book(locate_);
    if (!b) return 0;
    if (side_ > 0)   // selling out of a long: cross down through the bid
        return b->best_bid() > cfg_.cross_ticks ? b->best_bid() - cfg_.cross_ticks : 0;
    return b->best_ask() ? b->best_ask() + cfg_.cross_ticks : 0;
}

void OrderManager::submit_entry(int dir, float size_frac, uint64_t ts) {
    uint32_t px = entry_price(dir);
    if (px == 0) return;            // empty book: nothing to price against
    float eq = cash_;               // flat, so equity == cash
    uint32_t shares = (uint32_t)std::floor(eq * size_frac / (float)px);
    if (shares == 0) return;
    entry_dir_ = dir;
    entry_ref_ = fe_.submit_limit(dir > 0 ? SIDE_BUY : SIDE_SELL, px, shares, ts);
    poll_fills();                   // crossing entries fill immediately
    if (fe_.find(entry_ref_) == nullptr) entry_ref_ = 0;
}

void OrderManager::submit_exit(int reason, uint64_t ts) {
    if (side_ == 0 || qty_ == 0) {
        // position may be entirely unfilled entry: nothing to close
        return;
    }
    uint32_t px = exit_price();
    if (px == 0) return;
    exit_reason_ = reason;
    exit_ref_ = fe_.submit_limit(side_ > 0 ? SIDE_SELL : SIDE_BUY, px, qty_, ts);
    poll_fills();                   // crossing exit usually fills on the spot
}

void OrderManager::cancel_working_entry() {
    if (entry_ref_ == 0) return;
    fe_.cancel(entry_ref_);
    entry_ref_ = 0;
}

void OrderManager::finish(uint64_t ts) {
    cancel_working_entry();
    if (exit_ref_ != 0 && fe_.find(exit_ref_)) {
        fe_.cancel(exit_ref_);
        exit_ref_ = 0;
    }
    if (side_ != 0 && qty_ > 0) {
        // Synthetic mark-out at the last trade print (entry VWAP if the
        // stream never printed), taker fee — mirrors the batch engine's
        // end-of-data force close.
        uint32_t mark = last_print_ ? last_print_
                        : (uint32_t)(entry_notional_ / (float)entry_shares_);
        FillRec f{ts, 0, side_ > 0 ? SIDE_SELL : SIDE_BUY, mark, qty_, false};
        fills_log_.push_back(f);
        float notional = (float)mark * (float)qty_;
        float fee = notional * cfg_.fee;
        cash_ += (float)side_ * notional;
        cash_ -= fee;
        fees_open_ += fee;
        exit_shares_ += qty_;
        exit_notional_ += notional;
        qty_ = 0;
        close_round_trip(RSN_EOD);
    }
}

}  // namespace ob::trade
