// End-to-end RTL vs reference diff — the main correctness gate for the RTL.
//
// Replays a single-symbol ITCH file through the full Verilated orderbook_top
// pipeline. After every committed message it cross-checks the RTL book state
// (best bid/ask price+depth, total per-side volume, VWAP, trade count) against
// the self-contained std::map reference model, and every 1000 messages performs
// a full sweep of all active price levels. Exits non-zero on any mismatch.
#include "rtl_driver.hpp"
#include "reference_model.hpp"
#include "itch_file_reader.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace obsim;

static int g_fail = 0;
static const int MAX_REPORT = 20;

#define CHECK(cond, fmt, ...)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            if (g_fail < MAX_REPORT)                                           \
                std::printf("  MISMATCH @msg %llu: " fmt "\n",                 \
                            (unsigned long long)idx, ##__VA_ARGS__);           \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    std::string path = (argc > 1) ? argv[1] : "rtl_sim.itch";
    uint64_t    max_msgs = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 0;

    std::vector<uint8_t> raw = read_itch_file(path);
    if (raw.empty()) { std::printf("FAIL: cannot read %s\n", path.c_str()); return 2; }
    std::vector<RawMessage> msgs = split_messages(raw, max_msgs);
    std::vector<uint8_t>    stream = reserialize(msgs);

    std::printf("tb_top: replaying %zu messages from %s\n", msgs.size(), path.c_str());

    // Pre-decode reference messages (applied lockstep with RTL commits).
    std::vector<RefMsg> refmsgs;
    refmsgs.reserve(msgs.size());
    for (auto& m : msgs) refmsgs.push_back(decode_body(m.body));

    RefBook  ref;
    RtlDriver drv;
    drv.reset(/*raw_mode=*/true);

    auto* top = drv.top();

    auto on_commit = [&](uint64_t idx) {
        ref.apply(refmsgs[idx]);

        // top-of-book
        CHECK((bool)top->best_bid_valid == ref.best_bid_valid(),
              "best_bid_valid rtl=%d ref=%d", (int)top->best_bid_valid, ref.best_bid_valid());
        if (ref.best_bid_valid()) {
            CHECK(top->best_bid_price == ref.best_bid_price(),
                  "best_bid_price rtl=%u ref=%u", (unsigned)top->best_bid_price, ref.best_bid_price());
            CHECK(top->best_bid_shares == ref.best_bid_shares(),
                  "best_bid_shares rtl=%u ref=%llu", (unsigned)top->best_bid_shares,
                  (unsigned long long)ref.best_bid_shares());
        }
        CHECK((bool)top->best_ask_valid == ref.best_ask_valid(),
              "best_ask_valid rtl=%d ref=%d", (int)top->best_ask_valid, ref.best_ask_valid());
        if (ref.best_ask_valid()) {
            CHECK(top->best_ask_price == ref.best_ask_price(),
                  "best_ask_price rtl=%u ref=%u", (unsigned)top->best_ask_price, ref.best_ask_price());
            CHECK(top->best_ask_shares == ref.best_ask_shares(),
                  "best_ask_shares rtl=%u ref=%llu", (unsigned)top->best_ask_shares,
                  (unsigned long long)ref.best_ask_shares());
        }
        // aggregate volumes + stats
        CHECK(top->tot_bid_vol == ref.tot_bid_vol(),
              "tot_bid_vol rtl=%llu ref=%llu", (unsigned long long)top->tot_bid_vol,
              (unsigned long long)ref.tot_bid_vol());
        CHECK(top->tot_ask_vol == ref.tot_ask_vol(),
              "tot_ask_vol rtl=%llu ref=%llu", (unsigned long long)top->tot_ask_vol,
              (unsigned long long)ref.tot_ask_vol());
        CHECK(top->vwap_num == ref.vwap_num(),
              "vwap_num rtl=%llu ref=%llu", (unsigned long long)top->vwap_num,
              (unsigned long long)ref.vwap_num());
        CHECK(top->vwap_den == ref.vwap_den(),
              "vwap_den rtl=%llu ref=%llu", (unsigned long long)top->vwap_den,
              (unsigned long long)ref.vwap_den());
        CHECK(top->trade_count == ref.trade_count(),
              "trade_count rtl=%llu ref=%llu", (unsigned long long)top->trade_count,
              (unsigned long long)ref.trade_count());
        CHECK((bool)top->spread_valid == ref.spread_valid(),
              "spread_valid rtl=%d ref=%d", (int)top->spread_valid, ref.spread_valid());
        if (ref.spread_valid())
            CHECK(top->spread == ref.spread(), "spread rtl=%u ref=%u",
                  (unsigned)top->spread, ref.spread());

        // deep level sweep every 1000 messages
        if ((idx + 1) % 1000 == 0) {
            for (uint32_t p : ref.bid_prices())
                CHECK(drv.level_shares(true, p) == ref.level_shares(true, p),
                      "bid level %u shares rtl=%u ref=%llu", p,
                      drv.level_shares(true, p), (unsigned long long)ref.level_shares(true, p));
            for (uint32_t p : ref.ask_prices())
                CHECK(drv.level_shares(false, p) == ref.level_shares(false, p),
                      "ask level %u shares rtl=%u ref=%llu", p,
                      drv.level_shares(false, p), (unsigned long long)ref.level_shares(false, p));
        }
    };

    uint64_t committed = drv.drive(stream, msgs.size(), on_commit);

    std::printf("tb_top: committed %llu/%zu messages in %llu cycles\n",
                (unsigned long long)committed, msgs.size(),
                (unsigned long long)drv.cycles());
    std::printf("  RTL  best_bid=%u(%u) best_ask=%u(%u) vwap=%.4f trades=%llu\n",
                (unsigned)top->best_bid_price, (unsigned)top->best_bid_shares,
                (unsigned)top->best_ask_price, (unsigned)top->best_ask_shares,
                top->vwap_den ? double(top->vwap_num) / double(top->vwap_den) / 10000.0 : 0.0,
                (unsigned long long)top->trade_count);
    std::printf("  perf add_cyc=%llu del_cyc=%llu repl_cyc=%llu scans=%llu scan_cyc=%llu "
                "probe1=%llu probe2=%llu probegt2=%llu\n",
                (unsigned long long)top->add_cycles, (unsigned long long)top->delete_cycles,
                (unsigned long long)top->replace_cycles, (unsigned long long)top->scan_count,
                (unsigned long long)top->scan_cycles_total, (unsigned long long)top->hash_probe_1,
                (unsigned long long)top->hash_probe_2, (unsigned long long)top->hash_probe_gt2);

    if (committed != msgs.size()) {
        std::printf("FAIL: only %llu/%zu messages committed (pipeline hang?)\n",
                    (unsigned long long)committed, msgs.size());
        return 1;
    }
    if (g_fail) { std::printf("FAIL: %d mismatches\n", g_fail); return 1; }
    std::printf("PASS: RTL book matches reference across all %zu messages\n", msgs.size());
    return 0;
}
