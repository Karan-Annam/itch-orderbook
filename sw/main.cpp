// main.cpp — software order book: replay an ITCH file, maintain the book,
// measure per-message latency with RDTSC, and emit statistics + CSVs.
//
// Usage:
//   orderbook_sw [file=data/sample.itch] [--gen N] [--symbol L] [--csv DIR]
//                [--engine {simd|map}] [--ladder DIR] [--ladder-depth K]
//                [--ladder-every N] [--pin CORE] [--quiet] [--mold]
//
// --mold treats the input file as MoldUDP64 packets (tools/mold_wrap output):
// decapped up front (gap/stale accounting printed) so the replay path below
// is identical either way.
//
// --engine simd  (default) runs the direct-indexed SIMD book (BookEngine)
// --engine map             runs the std::map reference book (ReferenceBook)
// CSV prefix is latency_<engine>_*.csv so both can be compared side-by-side.
// --ladder DIR writes a ladder.csv (seq,side,level,price,shares) every --ladder-every
// messages; only available with --engine simd (the map book has equivalent accessors
// but the ladder is used for the depth-chart visualisation which uses the SIMD book).
#include "parser/itch_parser.hpp"
#include "parser/mold_parser.hpp"
#include "book/order_book.hpp"
#include "book/reference_book.hpp"
#include "stats/stats_engine.hpp"
#include "receiver/replay_receiver.hpp"
#include "receiver/afxdp_receiver.hpp"
#include "util/rdtsc.hpp"
#include "util/latency_hist.hpp"
#include "util/itch_gen.hpp"
#include "util/perf_events.hpp"
#include "util/cpu_affinity.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <memory>

namespace ob {
void write_hash_stats_csv(const char* path, const OrderRefTable& t);
}

using namespace ob;

static const char* kTypeNames[IDX_COUNT] = {
    "Add","AddMPID","Execute","ExecPrice","Cancel","Delete","Replace","Trade","System"};

int main(int argc, char** argv) {
    std::string file = "data/sample.itch";
    std::string csv_dir;
    std::string engine_mode = "simd";   // simd | map
    std::string ladder_dir;
    uint16_t primary      = 1;
    size_t   gen_n        = 0;
    int      ladder_depth = 10;
    int      ladder_every = 1000;
    bool     quiet        = false;
    bool     mold_in      = false;
    bool     final_state  = false;
    int      pin_core     = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--gen"          && i + 1 < argc) gen_n = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--symbol"       && i + 1 < argc) primary = uint16_t(std::atoi(argv[++i]));
        else if (a == "--csv"          && i + 1 < argc) csv_dir = argv[++i];
        else if (a == "--engine"       && i + 1 < argc) engine_mode = argv[++i];
        else if (a == "--ladder"       && i + 1 < argc) ladder_dir = argv[++i];
        else if (a == "--ladder-depth" && i + 1 < argc) ladder_depth = std::atoi(argv[++i]);
        else if (a == "--ladder-every" && i + 1 < argc) ladder_every = std::atoi(argv[++i]);
        else if (a == "--pin"          && i + 1 < argc) pin_core = std::atoi(argv[++i]);
        else if (a == "--mold")                          mold_in = true;
        else if (a == "--final-state")                   final_state = true;
        else if (a == "--quiet")                         quiet = true;
        else if (a[0] != '-')                            file = a;
    }

    if (engine_mode != "simd" && engine_mode != "map") {
        std::fprintf(stderr, "[error] --engine must be 'simd' or 'map'\n");
        return 1;
    }
    if (!ladder_dir.empty() && engine_mode != "simd") {
        std::fprintf(stderr, "[warn] --ladder is only supported with --engine simd; ignoring\n");
        ladder_dir.clear();
    }
    if (ladder_every < 1) ladder_every = 1;
    if (ladder_depth < 1) ladder_depth = 1;

    // Optional: pin to an isolated core + real-time priority (Linux; no-op
    // elsewhere) to remove scheduler jitter from the measured hot path.
    if (pin_core >= 0) {
        bool pinned = pin_to_core(pin_core);
        set_realtime_priority();
        std::printf("[affinity] pin core %d: %s\n", pin_core,
                    pinned ? "ok" : "unsupported on this platform");
    }

    // ---- acquire data -------------------------------------------------------
    FileReplayReceiver rx;
    if (gen_n > 0) {
        GenConfig cfg; cfg.num_symbols = 4;
        ItchGenerator g(cfg);
        rx.load_bytes(g.generate(gen_n));
        std::printf("[gen] synthesized %zu bytes (%zu messages)\n", rx.size(), gen_n);
    } else if (!rx.load(file)) {
        std::printf("[warn] could not open %s; synthesizing 100k messages\n", file.c_str());
        GenConfig cfg; cfg.num_symbols = 4;
        ItchGenerator g(cfg);
        rx.load_bytes(g.generate(100000));
    } else {
        std::printf("[load] %s: %zu bytes\n", file.c_str(), rx.size());
    }

    // --mold: decap MoldUDP64 packets to the plain length-prefixed stream up
    // front, so the measured replay path below stays identical either way.
    if (mold_in) {
        std::vector<uint8_t> plain;
        plain.reserve(rx.size());
        MoldStats ms = MoldParser::decap(rx.data(), rx.size(), plain);
        std::printf("[mold] %llu packets (%llu heartbeats, %llu stale), %llu msgs, "
                    "gaps: %llu events / %llu msgs lost, session_end=%d%s\n",
                    (unsigned long long)ms.packets, (unsigned long long)ms.heartbeats,
                    (unsigned long long)ms.stale_packets, (unsigned long long)ms.messages,
                    (unsigned long long)ms.gap_events, (unsigned long long)ms.gap_msgs,
                    ms.session_end ? 1 : 0, ms.truncated ? " [TRUNCATED]" : "");
        rx.load_bytes(plain);
    }

    // ---- setup --------------------------------------------------------------
    TscClock clk; clk.calibrate(80);
    std::printf("[tsc] %.3f ticks/ns  | SIMD tier: %s | feed backend: %s | engine: %s\n",
                clk.ticks_per_ns, simd_tier(), AFXDPReceiver::backend_name(),
                engine_mode.c_str());

    // Allocate exactly one of the two engines depending on the selected mode.
    // BookEngine is heavyweight (large hash table); ReferenceBook is heap-only.
    std::unique_ptr<BookEngine>    engine;
    std::unique_ptr<ReferenceBook> ref_book;
    if (engine_mode == "simd")
        engine   = std::make_unique<BookEngine>(1'000'000, 1u << 21);
    else
        ref_book = std::make_unique<ReferenceBook>();

    StatsEngine stats;  // VWAP/spread/imbalance — fed only in simd mode

    std::array<LatencyHist, IDX_COUNT> hist;
    for (auto& h : hist) h = LatencyHist(20000);
    LatencyHist all(20000);

    // book_depth.csv — every ladder_every messages, top-of-book snapshot
    FILE* depth_csv = nullptr;
    if (!csv_dir.empty()) {
        std::string p = csv_dir + "/book_depth.csv";
        depth_csv = std::fopen(p.c_str(), "w");
        if (depth_csv) std::fprintf(depth_csv,
            "seq,locate,best_bid,bid_depth,best_ask,ask_depth,spread\n");
    }

    // ladder.csv — depth ladder snapshots (simd mode only, written to ladder_dir)
    FILE* ladder_csv = nullptr;
    if (!ladder_dir.empty()) {
        std::string lp = ladder_dir + "/ladder.csv";
        ladder_csv = std::fopen(lp.c_str(), "w");
        if (ladder_csv) std::fprintf(ladder_csv, "seq,side,level,price,shares\n");
    }

    // ---- optional CPU performance counters (Linux perf_event_open) ----------
    PerfCounters perf;
    bool have_perf = perf.open();
    if (have_perf) perf.start();

    // ---- hot loop: decode + apply, measured per message ---------------------
    const uint8_t* data = rx.data();
    const size_t   len  = rx.size();
    size_t o = 0;
    uint64_t seq = 0;

    const bool is_simd = (engine_mode == "simd");

    while (o + 2 <= len) {
        uint16_t blen = be16(data + o);
        if (blen == 0 || o + 2 + blen > len) break;
        const uint8_t* body = data + o + 2;

        // Timed region: parse + book update ONLY, identical work for both
        // engines. stats.on_message() (VWAP/spread/imbalance) is deliberately
        // OUTSIDE the timer so the simd vs map comparison is apples-to-apples —
        // otherwise simd would be charged for stats work that map never runs.
        const uint64_t t0 = rdtscp();
        DecodedMessage m = ItchParser::decode_body(body, blen);
        BookEngine::ExecResult ex;
        if (is_simd) {
            engine->apply(m, &ex);
        } else {
            ref_book->apply(m);
        }
        const uint64_t t1 = rdtscp();

        if (is_simd) stats.on_message(m, ex);   // untimed: feeds VWAP/book_depth

        const double ns = clk.to_ns(t1 - t0);
        const int idx = msg_type_idx(m.type);
        hist[idx].record(ns);
        all.record(ns);

        ++seq;
        if (!quiet && (seq % 100000 == 0)) {
            if (is_simd) {
                const OrderBook* b = engine->book(primary);
                if (b) std::printf(
                    "  [%8llu] sym%u  bid %u x%u | ask %u x%u | spread %u | vwap $%.4f\n",
                    (unsigned long long)seq, primary,
                    b->best_bid(), b->best_bid_depth(),
                    b->best_ask(), b->best_ask_depth(),
                    b->spread(), stats.vwap_dollars());
            } else {
                std::printf(
                    "  [%8llu] sym%u  bid %u x%u | ask %u x%u\n",
                    (unsigned long long)seq, primary,
                    ref_book->best_bid(primary), ref_book->best_bid_depth(primary),
                    ref_book->best_ask(primary), ref_book->best_ask_depth(primary));
            }
        }

        // book_depth.csv snapshot
        if (depth_csv && (seq % 1000 == 0)) {
            if (is_simd) {
                const OrderBook* b = engine->book(primary);
                if (b) std::fprintf(depth_csv, "%llu,%u,%u,%u,%u,%u,%u\n",
                    (unsigned long long)seq, primary,
                    b->best_bid(), b->best_bid_depth(),
                    b->best_ask(), b->best_ask_depth(), b->spread());
            } else {
                uint32_t bb = ref_book->best_bid(primary);
                uint32_t ba = ref_book->best_ask(primary);
                uint32_t sp = (bb && ba && ba > bb) ? ba - bb : 0;
                std::fprintf(depth_csv, "%llu,%u,%u,%u,%u,%u,%u\n",
                    (unsigned long long)seq, primary,
                    bb, ref_book->best_bid_depth(primary),
                    ba, ref_book->best_ask_depth(primary), sp);
            }
        }

        // ladder.csv snapshot (simd only)
        if (ladder_csv && is_simd && (seq % ladder_every == 0)) {
            const OrderBook* b = engine->book(primary);
            if (b) {
                // bid: walk down from best_bid, collect top ladder_depth non-empty levels
                uint32_t p = b->best_bid();
                int lvl = 0, guard = 0;
                const int max_scan = ladder_depth * 200;
                while (p > 0 && lvl < ladder_depth && guard < max_scan) {
                    uint32_t sh = b->bid_shares_at(p);
                    if (sh > 0) {
                        ++lvl;
                        std::fprintf(ladder_csv, "%llu,bid,%d,%u,%u\n",
                            (unsigned long long)seq, lvl, p, sh);
                    }
                    --p; ++guard;
                }
                // ask: walk up from best_ask
                p = b->best_ask();
                lvl = 0; guard = 0;
                while (p > 0 && lvl < ladder_depth && guard < max_scan) {
                    uint32_t sh = b->ask_shares_at(p);
                    if (sh > 0) {
                        ++lvl;
                        std::fprintf(ladder_csv, "%llu,ask,%d,%u,%u\n",
                            (unsigned long long)seq, lvl, p, sh);
                    }
                    ++p; ++guard;
                }
            }
        }

        o += 2 + blen;
    }
    if (depth_csv)  std::fclose(depth_csv);
    if (ladder_csv) std::fclose(ladder_csv);

    PerfCounters::Snapshot perf_snap;
    if (have_perf) { perf_snap = perf.stop(); perf.close(); }

    // ---- report -------------------------------------------------------------
    std::printf("\n==== per-message latency (RDTSC, ns) [engine=%s] ====\n",
                engine_mode.c_str());
    for (int i = 0; i < IDX_COUNT; ++i)
        if (hist[i].count()) hist[i].print(kTypeNames[i]);
    all.print("ALL");

    std::printf("\n==== CPU performance counters ====\n");
    if (perf_snap.valid) {
        std::printf("  cache misses=%llu (%.3f/msg)  branch mispredicts=%llu (%.3f/msg)\n",
            (unsigned long long)perf_snap.cache_misses,
            seq ? double(perf_snap.cache_misses) / seq : 0.0,
            (unsigned long long)perf_snap.branch_mispredicts,
            seq ? double(perf_snap.branch_mispredicts) / seq : 0.0);
    } else {
        std::printf("  perf_event_open unavailable on this platform "
                    "(Linux-only; tail latency above reflects cache/OS jitter)\n");
    }

    if (is_simd) {
        const auto& c = engine->counters();
        std::printf("\n==== message counts ====\n");
        std::printf("  Add %llu  AddMPID(in Add path) | Exec %llu  Cancel %llu  "
                    "Delete %llu  Replace %llu  Trade %llu  System %llu\n",
                    (unsigned long long)c.add, (unsigned long long)c.exec,
                    (unsigned long long)c.cancel, (unsigned long long)c.del,
                    (unsigned long long)c.replace, (unsigned long long)c.trade,
                    (unsigned long long)c.sys);

        std::printf("\n==== order-ref hash table ====\n");
        const auto& hs = engine->refs().stats();
        std::printf("  size=%zu  load=%.3f  probe1=%llu probe2=%llu probe>2=%llu  "
                    "collision_rate=%.4f\n",
                    engine->refs().size(), engine->refs().load_factor(),
                    (unsigned long long)hs.probe1, (unsigned long long)hs.probe2,
                    (unsigned long long)hs.probe_gt2, hs.collision_rate());

        std::printf("\n==== best-price scans (Delete/Execute) ====\n");
        if (const OrderBook* b = engine->book(primary)) {
            std::printf("  sym%u: scans=%llu  total_scan_steps=%llu  avg_steps/scan=%.2f\n",
                primary, (unsigned long long)b->scan_count(),
                (unsigned long long)b->scan_steps(),
                b->scan_count() ? double(b->scan_steps()) / double(b->scan_count()) : 0.0);
        }

        std::printf("\n==== statistics (sym%u) ====\n", primary);
        if (const OrderBook* b = engine->book(primary)) {
            std::printf("  best bid %u (depth %u) | best ask %u (depth %u) | spread %u ticks\n",
                b->best_bid(), b->best_bid_depth(), b->best_ask(), b->best_ask_depth(), b->spread());
            std::printf("  imbalance(top5)=%.4f\n", StatsEngine::imbalance(*b, 5));
        }
        std::printf("  VWAP=$%.4f over %llu trades / %llu shares | avg msg rate=%.0f msg/s\n",
            stats.vwap_dollars(), (unsigned long long)stats.trade_count(),
            (unsigned long long)stats.trade_shares(), stats.avg_msg_rate());
    } else {
        std::printf("\n==== message counts ====\n");
        std::printf("  total messages processed: %llu\n", (unsigned long long)seq);
        std::printf("  (per-type counts not tracked in map mode)\n");

        std::printf("\n==== statistics (sym%u) ====\n", primary);
        uint32_t bb = ref_book->best_bid(primary);
        uint32_t ba = ref_book->best_ask(primary);
        std::printf("  best bid %u (depth %u) | best ask %u (depth %u) | live orders %zu\n",
            bb, ref_book->best_bid_depth(primary),
            ba, ref_book->best_ask_depth(primary),
            ref_book->live_orders());
    }

    std::printf("  total messages processed: %llu\n", (unsigned long long)seq);

    // --final-state: one machine-readable line for diffing against the RTL
    // banded replay and the hardware snapshot (tools/run_real_data.sh,
    // tools/ob_host.py verify). simd mode only (map mode lacks the stats).
    if (final_state && engine) {
        const OrderBook* b = engine->book(primary);
        std::printf("FINAL bid_px=%u bid_sh=%u ask_px=%u ask_sh=%u spread=%u "
                    "vwap_num=%llu vwap_den=%llu trades=%llu msgs=%llu\n",
                    b ? b->best_bid() : 0, b ? b->best_bid_depth() : 0,
                    b ? b->best_ask() : 0, b ? b->best_ask_depth() : 0,
                    b ? b->spread() : 0,
                    (unsigned long long)stats.vwap_numerator(),
                    (unsigned long long)stats.vwap_denominator(),
                    (unsigned long long)stats.trade_count(),
                    (unsigned long long)seq);
    }

    // ---- CSVs ---------------------------------------------------------------
    if (!csv_dir.empty()) {
        const std::string prefix = "latency_" + engine_mode + "_";
        for (int i = 0; i < IDX_COUNT; ++i) {
            if (!hist[i].count()) continue;
            std::string p = csv_dir + "/" + prefix + kTypeNames[i] + ".csv";
            hist[i].write_csv(p.c_str(), kTypeNames[i]);
        }
        std::string allp = csv_dir + "/" + prefix + "all.csv";
        all.write_csv(allp.c_str(), "ALL");

        if (is_simd) {
            std::string hp = csv_dir + "/hash_stats.csv";
            write_hash_stats_csv(hp.c_str(), engine->refs());
        }

        std::string pc = csv_dir + "/perf_counters_" + engine_mode + ".csv";
        if (FILE* f = std::fopen(pc.c_str(), "w")) {
            std::fprintf(f, "counter,value\n");
            std::fprintf(f, "messages,%llu\n", (unsigned long long)seq);
            std::fprintf(f, "p50_all_ns,%.0f\n", all.percentile(0.50));
            std::fprintf(f, "p99_all_ns,%.0f\n", all.percentile(0.99));
            if (is_simd) {
                const auto& c = engine->counters();
                std::fprintf(f, "add,%llu\n",     (unsigned long long)c.add);
                std::fprintf(f, "exec,%llu\n",    (unsigned long long)c.exec);
                std::fprintf(f, "cancel,%llu\n",  (unsigned long long)c.cancel);
                std::fprintf(f, "delete,%llu\n",  (unsigned long long)c.del);
                std::fprintf(f, "replace,%llu\n", (unsigned long long)c.replace);
                std::fprintf(f, "trade,%llu\n",   (unsigned long long)c.trade);
                std::fprintf(f, "vwap_units,%u\n", stats.vwap_units());
            }
            std::fclose(f);
        }
        std::printf("\n[csv] wrote latency / perf CSVs to %s/ (prefix: %s)\n",
                    csv_dir.c_str(), prefix.c_str());
        if (ladder_csv)
            std::printf("[csv] wrote %s/ladder.csv\n", ladder_dir.c_str());
    }
    return 0;
}
