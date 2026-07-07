// itch_gen.hpp — deterministic synthetic NASDAQ ITCH 5.0 stream generator.
//
// NASDAQ historical ITCH sample files are large and must be downloaded out of
// band (see data/README.md). To make the whole pipeline testable and
// reproducible without that download, this generator emits a byte-exact ITCH
// 5.0 stream — correct 2-byte length prefixes, big-endian fields, and the exact
// body layouts defined in itch_messages.hpp — covering every message type the
// book engine handles (A, F, E, C, X, D, U, P, S).
//
// Crucially, the generator maintains its own set of live orders, so every
// modify message (Execute/Cancel/Delete/Replace) refers to an order that is
// actually resting, and never over-executes. This yields a self-consistent
// stream: the reference book built from it never goes negative, which is what
// lets the std::map reference, the SIMD fast book, and the RTL all agree.
#pragma once

#include "../parser/itch_messages.hpp"
#include "endian.hpp"

#include <cstdint>
#include <vector>
#include <random>
#include <cstdio>
#include <array>

namespace ob {

struct GenConfig {
    uint64_t seed         = 0xC0FFEE12345ULL;
    int      num_symbols  = 4;
    uint32_t base_price   = 500000;   // $50.0000 in 1/10000 units (symbol 0 mid)
    uint32_t sym_spacing  = 50000;    // $5.00 between successive symbols' mids
    uint32_t tick         = 100;      // $0.01
    uint32_t band_ticks   = 100;      // resting orders span +/- this many ticks
    uint32_t max_price    = 999990;   // keep within a 1,000,000-entry book array
    uint32_t min_price    = 10000;    // $1.0000
    uint32_t max_shares   = 1000;
};

struct GenStats {
    std::array<uint64_t, IDX_COUNT> per_type{};
    uint64_t total = 0;
    uint64_t live_orders_peak = 0;
};

class ItchGenerator {
public:
    explicit ItchGenerator(const GenConfig& cfg = {})
        : cfg_(cfg), rng_(cfg.seed) {
        // Fixed, tick-aligned mid per symbol. Resting bids are placed strictly
        // below the mid and asks strictly above it, so the synthetic book is
        // never crossed (best_bid < best_ask always) — exactly as a real,
        // matched order book behaves — without needing a full matching engine.
        mids_.resize(cfg.num_symbols);
        for (int s = 0; s < cfg.num_symbols; ++s) {
            uint32_t m = cfg.base_price + uint32_t(s) * cfg.sym_spacing;
            mids_[s] = (m / cfg.tick) * cfg.tick;   // snap to tick
        }
    }

    // Generate `n` messages (plus a leading System 'O' and 'Q'), returning the
    // raw length-prefixed ITCH byte stream.
    std::vector<uint8_t> generate(size_t n) {
        out_.clear();
        stats_ = GenStats{};
        emit_system(EVT_START_MESSAGES);
        emit_system(EVT_START_HOURS);
        for (size_t i = 0; i < n; ++i) step();
        emit_system(EVT_END_HOURS);
        emit_system(EVT_END_MESSAGES);
        return out_;
    }

    const GenStats& stats() const { return stats_; }

    // Write the raw stream to a file.
    static bool write_file(const char* path, const std::vector<uint8_t>& bytes) {
        FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        return true;
    }

private:
    struct LiveOrder {
        uint64_t ref;
        uint16_t stock_locate;
        char     side;
        uint32_t price;
        uint32_t shares;
    };

    void step() {
        ts_ += 1 + (rng_() % 50);  // advance clock a few ns
        const int roll = int(rng_() % 100);
        // Bias toward Add early so there is inventory to modify.
        const bool need_inventory = live_.size() < 16;
        if (need_inventory || roll < 45)          emit_add(roll % 7 == 0);  // A or F
        else if (roll < 62)                        emit_execute(roll % 5 == 0); // E or C
        else if (roll < 74)                        emit_cancel();
        else if (roll < 86)                        emit_delete();
        else if (roll < 96)                        emit_replace();
        else                                       emit_trade();
        if (live_.size() > stats_.live_orders_peak)
            stats_.live_orders_peak = live_.size();
    }

    // Tick-aligned price on the correct side of the symbol's mid. Bids land in
    // [mid - band_ticks*tick, mid - tick]; asks in [mid + tick, mid + band_ticks*tick].
    // Distance from the mid is geometrically weighted so most resting size sits
    // near the touch (realistic depth profile) while still populating deep levels.
    uint32_t rand_price(int sym, char side) {
        uint32_t k = 1 + uint32_t(rng_() % cfg_.band_ticks);
        // bias toward the touch: take the min of two draws
        uint32_t k2 = 1 + uint32_t(rng_() % cfg_.band_ticks);
        if (k2 < k) k = k2;
        int64_t m = int64_t(mids_[sym]);
        int64_t p = (side == SIDE_BUY) ? m - int64_t(k) * cfg_.tick
                                       : m + int64_t(k) * cfg_.tick;
        if (p < int64_t(cfg_.min_price)) p = cfg_.min_price;
        if (p > int64_t(cfg_.max_price)) p = cfg_.max_price;
        return uint32_t(p);
    }

    // -- emitters ----------------------------------------------------------
    uint8_t* begin_msg(size_t body_len, char type, uint16_t locate) {
        const size_t start = out_.size();
        out_.resize(start + 2 + body_len, 0);
        uint8_t* msg = out_.data() + start;
        store_be16(msg, uint16_t(body_len));       // length prefix
        uint8_t* body = msg + 2;
        body[hdr::TYPE] = uint8_t(type);
        store_be16(body + hdr::STOCK_LOC, locate);
        store_be16(body + hdr::TRACK_NUM, uint16_t(track_++));
        store_be48(body + hdr::TIMESTAMP + 2, ts_); // 8-byte ts: high 2 bytes 0
        return body;
    }

    void count(MsgType t) { ++stats_.per_type[msg_type_idx(t)]; ++stats_.total; }

    void emit_system(char code) {
        uint8_t* b = begin_msg(blen::SYSTEM, char(MsgType::SystemEvent), 0);
        b[off::SYS_EVENT] = uint8_t(code);
        count(MsgType::SystemEvent);
    }

    void emit_add(bool mpid) {
        const int sym = int(rng_() % cfg_.num_symbols);
        const uint16_t locate = uint16_t(sym + 1);
        const char side = (rng_() & 1) ? SIDE_BUY : SIDE_SELL;
        const uint32_t price = rand_price(sym, side);
        const uint32_t shares = 1 + uint32_t(rng_() % cfg_.max_shares);
        const uint64_t ref = next_ref_++;

        const MsgType t = mpid ? MsgType::AddOrderMPID : MsgType::AddOrder;
        const size_t  bl = mpid ? blen::ADD_MPID : blen::ADD;
        uint8_t* b = begin_msg(bl, char(t), locate);
        store_be64(b + off::ADD_REF, ref);
        b[off::ADD_SIDE] = uint8_t(side);
        store_be32(b + off::ADD_SHARES, shares);
        write_stock(b + off::ADD_STOCK, sym);
        store_be32(b + off::ADD_PRICE, price);
        if (mpid) { const char m[4] = {'M','M','0',char('1'+sym)};
                    for (int i=0;i<4;++i) b[off::ADD_MPID+i]=uint8_t(m[i]); }
        live_.push_back({ref, locate, side, price, shares});
        count(t);
    }

    int pick_live() {
        if (live_.empty()) return -1;
        return int(rng_() % live_.size());
    }
    void remove_live(int i) {
        live_[i] = live_.back();
        live_.pop_back();
    }

    void emit_execute(bool with_price) {
        int i = pick_live();
        if (i < 0) { emit_add(false); return; }
        LiveOrder& o = live_[i];
        uint32_t ex = 1 + uint32_t(rng_() % o.shares);
        if (with_price) {
            uint8_t* b = begin_msg(blen::EXEC_PRICE, char(MsgType::OrderExecutedPrice), o.stock_locate);
            store_be64(b + off::EXECP_REF, o.ref);
            store_be32(b + off::EXECP_SHARES, ex);
            store_be64(b + off::EXECP_MATCH, match_++);
            b[off::EXECP_PRINTABLE] = 'Y';
            store_be32(b + off::EXECP_PRICE, o.price);
            count(MsgType::OrderExecutedPrice);
        } else {
            uint8_t* b = begin_msg(blen::EXEC, char(MsgType::OrderExecuted), o.stock_locate);
            store_be64(b + off::EXEC_REF, o.ref);
            store_be32(b + off::EXEC_SHARES, ex);
            store_be64(b + off::EXEC_MATCH, match_++);
            count(MsgType::OrderExecuted);
        }
        o.shares -= ex;
        if (o.shares == 0) remove_live(i);
    }

    void emit_cancel() {
        int i = pick_live();
        if (i < 0) { emit_add(false); return; }
        LiveOrder& o = live_[i];
        uint32_t cx = 1 + uint32_t(rng_() % o.shares);
        uint8_t* b = begin_msg(blen::CANCEL, char(MsgType::OrderCancel), o.stock_locate);
        store_be64(b + off::CANCEL_REF, o.ref);
        store_be32(b + off::CANCEL_SHARES, cx);
        count(MsgType::OrderCancel);
        o.shares -= cx;
        if (o.shares == 0) remove_live(i);
    }

    void emit_delete() {
        int i = pick_live();
        if (i < 0) { emit_add(false); return; }
        LiveOrder o = live_[i];
        uint8_t* b = begin_msg(blen::DELETE, char(MsgType::OrderDelete), o.stock_locate);
        store_be64(b + off::DELETE_REF, o.ref);
        count(MsgType::OrderDelete);
        remove_live(i);
    }

    void emit_replace() {
        int i = pick_live();
        if (i < 0) { emit_add(false); return; }
        LiveOrder& o = live_[i];
        const uint64_t new_ref = next_ref_++;
        const int sym = int(o.stock_locate) - 1;
        // Replacement rests on the SAME side as the original order.
        uint32_t new_price = rand_price(sym < 0 ? 0 : sym, o.side);
        uint32_t new_shares = 1 + uint32_t(rng_() % cfg_.max_shares);
        uint8_t* b = begin_msg(blen::REPLACE, char(MsgType::OrderReplace), o.stock_locate);
        store_be64(b + off::REPL_ORIG_REF, o.ref);
        store_be64(b + off::REPL_NEW_REF, new_ref);
        store_be32(b + off::REPL_SHARES, new_shares);
        store_be32(b + off::REPL_PRICE, new_price);
        count(MsgType::OrderReplace);
        // Update live set: old ref gone, new ref resting at new price/size.
        o.ref = new_ref; o.price = new_price; o.shares = new_shares;
    }

    void emit_trade() {
        const int sym = int(rng_() % cfg_.num_symbols);
        const uint16_t locate = uint16_t(sym + 1);
        const char side = (rng_() & 1) ? SIDE_BUY : SIDE_SELL;
        const uint32_t price = rand_price(sym, side);
        const uint32_t shares = 1 + uint32_t(rng_() % cfg_.max_shares);
        uint8_t* b = begin_msg(blen::TRADE, char(MsgType::Trade), locate);
        store_be64(b + off::TRADE_REF, 0);  // non-displayable: ref 0
        b[off::TRADE_SIDE] = uint8_t(side);
        store_be32(b + off::TRADE_SHARES, shares);
        write_stock(b + off::TRADE_STOCK, sym);
        store_be32(b + off::TRADE_PRICE, price);
        store_be64(b + off::TRADE_MATCH, match_++);
        count(MsgType::Trade);
    }

    void write_stock(uint8_t* p, int sym) {
        // 8-char ASCII, space-padded, e.g. "SYM1    ".
        char s[8] = {'S','Y','M',char('1'+sym),' ',' ',' ',' '};
        for (int i = 0; i < 8; ++i) p[i] = uint8_t(s[i]);
    }

    GenConfig             cfg_;
    std::mt19937_64       rng_;
    std::vector<uint8_t>  out_;
    std::vector<LiveOrder> live_;
    std::vector<uint32_t> mids_;
    GenStats              stats_;
    uint64_t              next_ref_ = 1;
    uint64_t              match_    = 1;
    uint64_t              ts_       = 34200ULL * 1000000000ULL; // 09:30:00 in ns
    uint16_t              track_    = 0;
};

}  // namespace ob
