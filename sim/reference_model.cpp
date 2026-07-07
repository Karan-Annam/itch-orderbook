#include "reference_model.hpp"

namespace obsim {

// ---- big-endian field loads (body-relative offsets) -----------------------
static uint16_t be16(const std::vector<uint8_t>& b, size_t k) {
    return (uint16_t(b[k]) << 8) | uint16_t(b[k + 1]);
}
static uint32_t be32(const std::vector<uint8_t>& b, size_t k) {
    return (uint32_t(b[k]) << 24) | (uint32_t(b[k + 1]) << 16) |
           (uint32_t(b[k + 2]) << 8) | uint32_t(b[k + 3]);
}
static uint64_t be64(const std::vector<uint8_t>& b, size_t k) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | b[k + i];
    return v;
}

// Spec field offsets (mirror rtl/ob_pkg.sv off:: namespace).
RefMsg decode_body(const std::vector<uint8_t>& b) {
    RefMsg m;
    if (b.empty()) return m;
    m.type   = b[0];
    m.locate = be16(b, 1);
    switch (m.type) {
        case 'A': case 'F':
            m.order_ref = be64(b, 13);
            m.is_bid    = (b[21] == 'B');
            m.shares    = be32(b, 22);
            m.price     = be32(b, 34);
            break;
        case 'E':
            m.order_ref = be64(b, 13);
            m.shares    = be32(b, 21);
            break;
        case 'C':
            m.order_ref = be64(b, 13);
            m.shares    = be32(b, 21);
            m.printable = (b[33] == 'Y');
            m.price     = be32(b, 34);
            break;
        case 'X':
            m.order_ref = be64(b, 13);
            m.shares    = be32(b, 21);
            break;
        case 'D':
            m.order_ref = be64(b, 13);
            break;
        case 'U':
            m.order_ref     = be64(b, 13);
            m.new_order_ref = be64(b, 21);
            m.shares        = be32(b, 29);
            m.price         = be32(b, 33);
            break;
        case 'P':
            m.order_ref = be64(b, 13);
            m.is_bid    = (b[21] == 'B');
            m.shares    = be32(b, 22);
            m.price     = be32(b, 34);
            break;
        case 'S':
        default:
            break;
    }
    return m;
}

// ---- book mutations -------------------------------------------------------
void RefBook::add_order(uint64_t ref, bool is_bid, uint16_t locate,
                        uint32_t price, uint64_t shares) {
    orders_[ref] = Order{is_bid, locate, price, shares};
    auto& m = is_bid ? bids_ : asks_;
    Level& lv = m[price];
    lv.shares += shares;
    lv.count  += 1;
    (is_bid ? tot_bid_ : tot_ask_) += shares;
}

void RefBook::reduce_order(uint64_t ref, uint64_t amt) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return;
    Order& o = it->second;
    if (amt > o.shares) amt = o.shares;
    auto& m = o.is_bid ? bids_ : asks_;
    auto lit = m.find(o.price);
    if (lit != m.end()) {
        lit->second.shares -= amt;
        (o.is_bid ? tot_bid_ : tot_ask_) -= amt;
        o.shares -= amt;
        if (o.shares == 0) {
            if (lit->second.count > 0) lit->second.count -= 1;
            if (lit->second.shares == 0) m.erase(lit);
            orders_.erase(it);
        }
    }
}

void RefBook::remove_order(uint64_t ref) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return;
    Order& o = it->second;
    auto& m = o.is_bid ? bids_ : asks_;
    auto lit = m.find(o.price);
    if (lit != m.end()) {
        lit->second.shares -= o.shares;
        if (lit->second.count > 0) lit->second.count -= 1;
        (o.is_bid ? tot_bid_ : tot_ask_) -= o.shares;
        if (lit->second.shares == 0) m.erase(lit);
    }
    orders_.erase(it);
}

void RefBook::apply(const RefMsg& m) {
    switch (m.type) {
        case 'A': case 'F':
            add_order(m.order_ref, m.is_bid, m.locate, m.price, m.shares);
            break;
        case 'E': {
            auto it = orders_.find(m.order_ref);
            if (it == orders_.end()) break;
            uint64_t amt = m.shares;
            uint32_t resting_price = it->second.price;
            if (amt > it->second.shares) amt = it->second.shares;
            reduce_order(m.order_ref, amt);
            vwap_num_ += uint64_t(resting_price) * amt;  // VWAP at resting price
            vwap_den_ += amt;
            trade_count_ += 1;
            break;
        }
        case 'C': {
            auto it = orders_.find(m.order_ref);
            if (it == orders_.end()) break;
            uint64_t amt = m.shares;
            if (amt > it->second.shares) amt = it->second.shares;
            reduce_order(m.order_ref, amt);
            if (m.printable) {                            // VWAP at exec price
                vwap_num_ += uint64_t(m.price) * amt;
                vwap_den_ += amt;
                trade_count_ += 1;
            }
            break;
        }
        case 'X': {
            auto it = orders_.find(m.order_ref);
            if (it == orders_.end()) break;
            uint64_t amt = m.shares;
            if (amt > it->second.shares) amt = it->second.shares;
            reduce_order(m.order_ref, amt);
            break;
        }
        case 'D':
            remove_order(m.order_ref);
            break;
        case 'U': {
            auto it = orders_.find(m.order_ref);
            if (it == orders_.end()) break;
            bool     side   = it->second.is_bid;
            uint16_t locate = it->second.locate;
            remove_order(m.order_ref);
            add_order(m.new_order_ref, side, locate, m.price, m.shares);
            break;
        }
        case 'P':
            vwap_num_ += uint64_t(m.price) * m.shares;
            vwap_den_ += m.shares;
            trade_count_ += 1;
            break;
        case 'S':
        default:
            break;
    }
}

}  // namespace obsim
