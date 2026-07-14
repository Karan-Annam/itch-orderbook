// Time-bucketed OHLCV bars from ITCH trade prints.
//
// The strategy VM consumes bar arrays with the exact indexing semantics of
// backtest_project (s[t-lag] clamped to 0, raw-window re-scans), so completed
// bars accumulate in growing per-series vectors and are never mutated after
// completion. Prices stay in raw ITCH units (1e-4 dollars) — they are exact in
// float32 up to 2^24 and the VM only ever compares/combines them relatively.
//
// Bucketing: bar index = timestamp_ns / bar_ns, anchored at the first trade's
// bucket. A trade in a later bucket closes the current bar; empty buckets in
// between become flat bars (o=h=l=c=prev close, v=0) so the bar clock never
// stalls and indicator windows stay time-uniform. Out-of-order trades (bucket
// earlier than the open bar) are folded into the open bar: book replay is
// sequenced upstream, so this only absorbs same-tick reordering.
#pragma once

#include <cstdint>
#include <vector>

namespace ob::trade {

struct BarSeries {
    std::vector<float> o, h, l, c, v;
    int size() const { return (int)c.size(); }
};

class BarBuilder {
public:
    explicit BarBuilder(uint64_t bar_ns) : bar_ns_(bar_ns) {}

    // Feed one trade print; returns how many bars this completed (0+).
    int on_trade(uint64_t ts_ns, uint32_t price_raw, uint32_t shares) {
        float px = (float)price_raw;
        int64_t bucket = (int64_t)(ts_ns / bar_ns_);
        if (!open_) {
            open_ = true;
            cur_bucket_ = bucket;
            o_ = h_ = l_ = c_ = px;
            vol_ = (float)shares;
            return 0;
        }
        if (bucket <= cur_bucket_) {  // same bucket, or late tick: fold in
            if (px > h_) h_ = px;
            if (px < l_) l_ = px;
            c_ = px;
            vol_ += (float)shares;
            return 0;
        }
        int completed = 0;
        emit(o_, h_, l_, c_, vol_);
        ++completed;
        for (int64_t b = cur_bucket_ + 1; b < bucket; ++b) {  // gap: flat bars
            emit(c_, c_, c_, c_, 0.0f);
            ++completed;
        }
        cur_bucket_ = bucket;
        o_ = h_ = l_ = px;  // new bar opens at the trade that closed the gap
        c_ = px;
        vol_ = (float)shares;
        return completed;
    }

    // Close the in-progress bar at stream end; returns 1 if one was emitted.
    int flush() {
        if (!open_) return 0;
        emit(o_, h_, l_, c_, vol_);
        open_ = false;
        return 1;
    }

    const BarSeries& bars() const { return bars_; }
    bool has_open_bar() const { return open_; }

private:
    void emit(float o, float h, float l, float c, float v) {
        bars_.o.push_back(o);
        bars_.h.push_back(h);
        bars_.l.push_back(l);
        bars_.c.push_back(c);
        bars_.v.push_back(v);
    }

    uint64_t bar_ns_;
    int64_t cur_bucket_ = 0;
    float o_ = 0, h_ = 0, l_ = 0, c_ = 0, vol_ = 0;
    bool open_ = false;
    BarSeries bars_;
};

}  // namespace ob::trade
