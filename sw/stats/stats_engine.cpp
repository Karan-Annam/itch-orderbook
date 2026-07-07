// stats_engine.cpp — StatsEngine::on_message.
#include "stats_engine.hpp"

namespace ob {

void StatsEngine::on_message(const DecodedMessage& m, const BookEngine::ExecResult& ex) {
    ++total_messages_;

    // Message-rate windows use ITCH event time (ns since midnight).
    const uint64_t ts = m.timestamp;
    if (ts) {
        if (first_ts_ == 0) first_ts_ = ts;
        last_ts_ = ts;
        window_.push_back(ts);
        while (!window_.empty() && window_.front() + ONE_SEC_NS <= ts)
            window_.pop_front();
    }

    // VWAP + trade count from resolved trade prints.
    if (ex.contributes && ex.shares > 0) {
        vwap_num_ += uint64_t(ex.price) * uint64_t(ex.shares);
        vwap_den_ += ex.shares;
        ++trade_count_;
    }
}

}  // namespace ob
