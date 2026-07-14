// One trading session over a raw ITCH byte stream: the orchestration loop
// shared by trade_main and the end-to-end test.
//
// Per message: FillEngine sees it first (pre-apply ref resolution), the book
// applies it, fills are polled into the OrderManager, and trade prints for
// the traded symbol drive protective-exit monitoring and the bar clock. Each
// completed bar steps the strategy VM (with the manager's live position/
// equity as context) and hands the signals back for order management.
#pragma once

#include "../book/order_book.hpp"
#include "../parser/itch_parser.hpp"
#include "../util/endian.hpp"
#include "bar_builder.hpp"
#include "dsl_vm.hpp"
#include "fill_engine.hpp"
#include "order_manager.hpp"
#include "program.hpp"

#include <cstdint>
#include <vector>

namespace ob::trade {

struct SessionConfig {
    uint16_t locate = 1;
    uint64_t bar_ns = 1'000'000'000;   // 1s bars
    OmConfig om;
    std::vector<float> params;         // empty = strategy defaults
};

struct SessionResult {
    BarSeries bars;
    std::vector<float> equity;         // marked at each completed bar's close
    std::vector<TradeRecT> trades;
    std::vector<FillRec> fills;
    float final_equity = 0.0f;
    float total_return = 0.0f;
    float max_dd = 0.0f;
    int maker_fills = 0, taker_fills = 0;
    int wins = 0;
    uint64_t messages = 0;
};

inline SessionResult run_session(const uint8_t* data, size_t len,
                                 const Program& prog, const SessionConfig& sc) {
    BookEngine eng;
    FillEngine fe(eng, sc.locate);
    OrderManager om(fe, eng, sc.locate, sc.om);
    BarBuilder bb(sc.bar_ns);
    StrategyVM vm(prog, sc.params);

    SessionResult res;
    int next_bar = 0;
    uint64_t last_ts = 0;

    auto process_bars = [&](uint64_t ts) {
        const BarSeries& bars = bb.bars();
        for (; next_bar < bars.size(); ++next_bar) {
            VmCtx ctx = om.ctx(bars, next_bar);
            Signals sig = vm.step(bars, next_bar, ctx);
            om.on_bar(bars, next_bar, sig, ts);
            res.equity.push_back(om.equity(bars.c[next_bar]));
        }
    };

    size_t o = 0;
    while (o + 2 <= len) {
        const uint16_t blen = be16(data + o);
        if (o + 2 + blen > len) break;
        const uint8_t* body = data + o + 2;
        o += 2 + size_t(blen);

        DecodedMessage m = ItchParser::decode_body(body, blen);
        ++res.messages;
        last_ts = m.timestamp;

        fe.on_market_message(m);
        BookEngine::ExecResult er;
        eng.apply(m, &er);
        om.poll_fills();

        if (er.contributes && er.locate == sc.locate) {
            om.on_trade_print(er.price, m.timestamp);
            if (bb.on_trade(m.timestamp, er.price, er.shares) > 0)
                process_bars(m.timestamp);
        }
    }
    if (bb.flush() > 0) process_bars(last_ts);
    om.finish(last_ts);

    res.bars = bb.bars();
    res.trades = om.trades();
    res.fills = om.fills();
    for (const FillRec& f : res.fills) (f.maker ? res.maker_fills : res.taker_fills)++;
    for (const TradeRecT& t : res.trades) if (t.pnl > 0.0f) res.wins++;

    res.final_equity = om.cash();      // flat after finish()
    res.total_return = sc.om.equity0 != 0.0f
                           ? res.final_equity / sc.om.equity0 - 1.0f : 0.0f;
    float peak = 0.0f;
    for (float e : res.equity) {
        if (e > peak) peak = e;
        if (peak > 0.0f) {
            float dd = (peak - e) / peak;
            if (dd > res.max_dd) res.max_dd = dd;
        }
    }
    return res;
}

}  // namespace ob::trade
