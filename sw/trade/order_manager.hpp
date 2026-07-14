// Signals -> orders, position, protective exits, PnL.
//
// Mirrors the batch broker's arbitration (backtest_project engine/vm_core.h):
// an exit signal while positioned closes; entry signals only act when flat,
// sized floor(equity * size / px) whole shares; conflicting entry signals
// cancel out. Where the batch broker fills analytically at next-bar-open with
// fixed slippage, this one routes real orders through the FillEngine:
//   * entries: --entry join (passive limit at the same-side best — the queue
//     model showcase) or --entry cross (limit through the opposite best).
//   * exits (signal, stop, trail, tp, eod): always cross, opposite best plus
//     a tick buffer, so they sweep displayed liquidity immediately.
// Protective exits are monitored against every trade print, not just bar
// closes; the trail anchor also advances per print. Passive entries re-peg at
// bar boundaries only (deterministic): if the touch moved, cancel/replace at
// the new best, resetting queue position.
//
// Prices are raw ITCH units (1e-4 dollars); cash/equity live in the same
// units. Fees are a fraction of notional per fill, charged to cash on the
// spot and folded into the round-trip TradeRecT pnl.
#pragma once

#include "dsl_vm.hpp"
#include "fill_engine.hpp"

#include <cstdint>
#include <vector>

namespace ob::trade {

// Trade exit reasons — codes match vm_core.h so reports compare 1:1.
enum { RSN_SIGNAL = 0, RSN_STOP = 1, RSN_TRAIL = 2, RSN_TP = 3, RSN_EOD = 4 };
const char* reason_name(int r);

struct TradeRecT {
    int side = 0;           // +1 long, -1 short
    int entry_t = 0;        // bar index of first entry fill
    int exit_t = 0;         // bar index position went flat
    float entry_px = 0.0f;  // VWAP of entry fills
    float exit_px = 0.0f;   // VWAP of exit fills
    float qty = 0.0f;
    float pnl = 0.0f;       // net of both legs' fees
    int reason = RSN_SIGNAL;
};

struct OmConfig {
    float fee = 0.001f;         // fraction of notional per fill
    float equity0 = 1e9f;       // raw price units ($100k at $1 == 1e4)
    bool entry_cross = false;   // false: join the touch; true: cross the spread
    uint32_t cross_ticks = 2;   // how far through the opposite best exits go
};

class OrderManager {
public:
    OrderManager(FillEngine& fe, BookEngine& eng, uint16_t locate, OmConfig cfg)
        : fe_(fe), eng_(eng), locate_(locate), cfg_(cfg), cash_(cfg.equity0) {}

    // Drain FillEngine fills into position/cash. Call after every message.
    void poll_fills();

    // Trade print (ExecResult with contributes): protective-exit monitoring.
    void on_trade_print(uint32_t price, uint64_t ts);

    // Completed bar t: VM context for the strategy step.
    VmCtx ctx(const BarSeries& bars, int t) const;

    // Completed bar t's signals: arbitration + order management + re-pegging.
    void on_bar(const BarSeries& bars, int t, const Signals& sig, uint64_t ts);

    // Stream end: cancel workings, force-close at the last print (RSN_EOD).
    void finish(uint64_t ts);

    float equity(float mark_px) const {
        return cash_ + (float)side_ * (float)qty_ * mark_px;
    }
    int position() const { return side_; }
    const std::vector<TradeRecT>& trades() const { return trades_; }
    const std::vector<FillRec>& fills() const { return fills_log_; }
    float cash() const { return cash_; }

private:
    void submit_entry(int dir, float size_frac, uint64_t ts);
    void submit_exit(int reason, uint64_t ts);
    void cancel_working_entry();
    uint32_t entry_price(int dir) const;   // per cfg_.entry_cross
    uint32_t exit_price() const;           // crossing limit for current side
    void apply_fill(const FillRec& f);
    void close_round_trip(int reason);

    FillEngine& fe_;
    BookEngine& eng_;
    uint16_t locate_;
    OmConfig cfg_;

    // position
    float cash_;
    int side_ = 0;              // -1/0/+1
    uint32_t qty_ = 0;          // shares held
    float entry_notional_ = 0;  // sum(px*sh) of entry fills
    uint32_t entry_shares_ = 0;
    float fees_open_ = 0;       // both legs' fees for the open round trip
    float exit_notional_ = 0;
    uint32_t exit_shares_ = 0;
    int entry_bar_ = -1;
    float anchor_ = 0;          // trail: peak (long) / trough (short)
    uint32_t last_print_ = 0;

    // working orders
    uint64_t entry_ref_ = 0;
    int entry_dir_ = 0;
    uint64_t exit_ref_ = 0;
    int exit_reason_ = RSN_SIGNAL;

    // config latched from the last bar's signals
    float cfg_stop_ = 0, cfg_tp_ = 0, cfg_trail_ = 0;

    int cur_bar_ = 0;           // bar index assigned to fills
    std::vector<TradeRecT> trades_;
    std::vector<FillRec> fills_log_;
};

}  // namespace ob::trade
