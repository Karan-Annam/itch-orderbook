// test_full_pipeline.cpp — bounded end-to-end replay vs reference (spec Phase 6).
//
// A compact sibling of tb_top.cpp: replays a bounded prefix of the shared ITCH
// file through the full Verilated pipeline and diffs top-of-book, aggregate
// volumes, and VWAP/trade-count against the std::map reference model at every
// commit, with a deep all-levels sweep at the end. Exits nonzero on any
// mismatch or pipeline hang.
#include "scenario.hpp"
#include "../itch_file_reader.hpp"

using namespace obsim;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "full_pipeline"};

    std::string path = (argc > 1) ? argv[1] : "rtl_sim.itch";
    uint64_t    cap  = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5000;

    std::vector<uint8_t> raw = read_itch_file(path);
    if (raw.empty()) { std::printf("[full_pipeline] cannot read %s\n", path.c_str()); return 2; }
    auto msgs = split_messages(raw, cap);
    auto stream = reserialize(msgs);
    std::vector<RefMsg> rm; rm.reserve(msgs.size());
    for (auto& m : msgs) rm.push_back(decode_body(m.body));

    RefBook ref; RtlDriver drv; drv.reset(true);
    uint64_t total = msgs.size();
    uint64_t committed = run_stream(drv, stream, total, [&](uint64_t idx){
        ref.apply(rm[idx]);
        auto* top = drv.top();
        SCHECK(ctx, (bool)top->best_bid_valid == ref.best_bid_valid(),
               "@%llu bid_valid", (unsigned long long)idx);
        if (ref.best_bid_valid())
            SCHECK(ctx, top->best_bid_price == ref.best_bid_price() &&
                        top->best_bid_shares == ref.best_bid_shares(),
                   "@%llu best_bid rtl=%u(%u) ref=%u(%llu)", (unsigned long long)idx,
                   (unsigned)top->best_bid_price, (unsigned)top->best_bid_shares,
                   ref.best_bid_price(), (unsigned long long)ref.best_bid_shares());
        if (ref.best_ask_valid())
            SCHECK(ctx, top->best_ask_price == ref.best_ask_price() &&
                        top->best_ask_shares == ref.best_ask_shares(),
                   "@%llu best_ask rtl=%u(%u) ref=%u(%llu)", (unsigned long long)idx,
                   (unsigned)top->best_ask_price, (unsigned)top->best_ask_shares,
                   ref.best_ask_price(), (unsigned long long)ref.best_ask_shares());
        SCHECK(ctx, top->vwap_num == ref.vwap_num() && top->vwap_den == ref.vwap_den(),
               "@%llu vwap rtl=%llu/%llu ref=%llu/%llu", (unsigned long long)idx,
               (unsigned long long)top->vwap_num, (unsigned long long)top->vwap_den,
               (unsigned long long)ref.vwap_num(), (unsigned long long)ref.vwap_den());
    });
    SCHECK(ctx, committed == total, "committed %llu/%llu (hang?)",
           (unsigned long long)committed, (unsigned long long)total);

    // Deep sweep over all active price levels at the end.
    for (uint32_t p : ref.bid_prices())
        SCHECK(ctx, drv.level_shares(true, p) == ref.level_shares(true, p),
               "final bid level %u rtl=%u ref=%llu", p, drv.level_shares(true, p),
               (unsigned long long)ref.level_shares(true, p));
    for (uint32_t p : ref.ask_prices())
        SCHECK(ctx, drv.level_shares(false, p) == ref.level_shares(false, p),
               "final ask level %u rtl=%u ref=%llu", p, drv.level_shares(false, p),
               (unsigned long long)ref.level_shares(false, p));

    std::printf("[full_pipeline] replayed %llu msgs: %d checks, %d failures\n",
                (unsigned long long)committed, ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
