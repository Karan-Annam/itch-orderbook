// test_overlap.cpp — ingest/engine decoupling.
//
// With the framer gating msg_complete on the decoder's pending flag (instead
// of waiting for engine_done), message N+1 should stream in and be fully
// assembled while the engine is still applying message N. Two properties:
//
//  1. Direct overlap proof: for most messages, the word carrying message
//     N+1's last byte is accepted BEFORE message N commits.
//  2. Throughput is engine-bound: commit-to-commit spacing for a pure Add
//     stream must be far below what serialized byte- or even word-ingest
//     would force (Add engine occupancy is ~8 cycles; the 40-byte frame
//     alone would cost 40 cycles serialized byte-wide).
#include "scenario.hpp"

using namespace obsim;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "overlap"};

    const int N = 200;
    std::vector<uint8_t> s;
    std::vector<size_t> frame_len;
    for (int j = 0; j < N; ++j) {
        auto m = add(2000 + j, 'B', 10, uint32_t(70000 + j), 1);
        frame_len.push_back(m.size());
        append(s, m);
    }
    RefBook ref;
    for (auto& m : split_messages(s, 0)) ref.apply(decode_body(m.body));

    RtlDriver drv; drv.reset(true);
    std::vector<uint64_t> ingest_done(N, 0), commit(N, 0);
    std::vector<uint64_t> service;
    uint64_t prev_commit = 0;
    uint64_t c = drv.drive_latency(s, frame_len,
        [&](uint64_t idx, uint64_t, uint64_t done_cyc, uint64_t commit_cyc) {
            ingest_done[idx] = done_cyc;
            commit[idx]      = commit_cyc;
            if (idx > 0) service.push_back(commit_cyc - prev_commit);
            prev_commit = commit_cyc;
        });
    SCHECK(ctx, c == uint64_t(N), "committed %llu/%d", (unsigned long long)c, N);

    // 1. overlap: N+1 fully ingested before N commits, for >90% of messages.
    int overlapped = 0;
    for (int j = 0; j + 1 < N; ++j)
        if (ingest_done[j + 1] && ingest_done[j + 1] < commit[j]) ++overlapped;
    SCHECK(ctx, overlapped > (N - 1) * 9 / 10,
           "only %d/%d messages had their successor pre-ingested", overlapped, N - 1);

    // 2. engine-bound service time: median commit-to-commit well under the
    //    40-cycle frame cost that serialized byte ingest would impose.
    std::vector<uint64_t> sorted = service;
    std::sort(sorted.begin(), sorted.end());
    uint64_t med = sorted[sorted.size() / 2];
    SCHECK(ctx, med <= 14, "median service %llu cycles, expected <= 14",
           (unsigned long long)med);

    // book must still be exactly right
    auto* t = drv.top();
    SCHECK(ctx, t->best_bid_price == ref.best_bid_price(), "best_bid rtl=%u ref=%u",
           (unsigned)t->best_bid_price, ref.best_bid_price());
    SCHECK(ctx, t->tot_bid_vol == ref.tot_bid_vol(), "tot_bid rtl=%llu ref=%llu",
           (unsigned long long)t->tot_bid_vol, (unsigned long long)ref.tot_bid_vol());

    std::printf("[overlap] overlapped=%d/%d median_service=%llu cyc | %d checks, %d failures\n",
                overlapped, N - 1, (unsigned long long)med, ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
