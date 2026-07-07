// tb_latency.cpp — RTL per-message latency measurement via Verilator.
//
// Replays a project-format ITCH file through Verilated orderbook_top and
// records the cycles consumed per message (commit-to-commit), converted to
// nanoseconds at a user-set clock frequency. Emits:
//   latency_hw_<Type>.csv  — per-type histograms (same schema as SW CSVs)
//   latency_hw_all.csv     — aggregated histogram
//   perf_counters_hw.csv   — RTL hardware perf counter final values
//
// Usage:
//   tb_latency <itch_file> [--csv DIR] [--mhz F] [--max N] [--quiet]
//
//   --mhz F  Clock frequency for cycle-to-ns conversion (default 250 MHz).
//   --max N  Stop after N messages (0 = all).
//   --quiet  Suppress per-batch progress prints.
#include "rtl_driver.hpp"
#include "itch_file_reader.hpp"
#include "Vorderbook_top.h"
#include "verilated.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <array>

using namespace obsim;

// ---- message type helpers (mirrors sw/parser/itch_messages.hpp) ------------

static const int TYPE_COUNT = 9;
static const char* TYPE_NAMES[TYPE_COUNT] = {
    "Add","AddMPID","Execute","ExecPrice","Cancel","Delete","Replace","Trade","System"
};

static int type_byte_to_idx(uint8_t c) {
    switch (c) {
        case 'A': return 0;  case 'F': return 1;  case 'E': return 2;
        case 'C': return 3;  case 'X': return 4;  case 'D': return 5;
        case 'U': return 6;  case 'P': return 7;  case 'S': return 8;
        default:  return -1;
    }
}

// ---- minimal latency histogram (same output schema as LatencyHist) ---------

struct Hist {
    static const int RANGE = 4000;   // 4 µs max — RTL is deterministic & fast
    uint64_t bins[RANGE + 1];
    uint64_t count;
    double   sum_ns;
    double   max_ns;

    Hist() : count(0), sum_ns(0.0), max_ns(0.0) {
        for (int i = 0; i <= RANGE; ++i) bins[i] = 0;
    }

    void record(double ns) {
        if (ns < 0) ns = 0;
        int idx = (ns >= RANGE) ? RANGE : static_cast<int>(ns);
        ++bins[idx];
        ++count;
        sum_ns += ns;
        if (ns > max_ns) max_ns = ns;
    }

    double percentile(double p) const {
        if (!count) return 0.0;
        uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(count)));
        uint64_t cum = 0;
        for (int i = 0; i <= RANGE; ++i) {
            cum += bins[i];
            if (cum >= target) return static_cast<double>(i);
        }
        return static_cast<double>(RANGE);
    }

    void print(const char* label) const {
        if (!count) return;
        std::printf(
            "  %-22s n=%-9llu  p50=%6.0f  p95=%6.0f  p99=%6.0f  p99.9=%6.0f  "
            "p99.99=%6.0f  max=%7.0f  mean=%6.1f  (ns)\n",
            label, (unsigned long long)count,
            percentile(0.50), percentile(0.95), percentile(0.99),
            percentile(0.999), percentile(0.9999), max_ns,
            count ? sum_ns / count : 0.0);
    }

    // Same CSV schema as LatencyHist::write_csv: msg_type,latency_ns,count
    void write_csv(const char* path, const char* msg_type) const {
        FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::fprintf(f, "msg_type,latency_ns,count\n");
        for (int i = 0; i <= RANGE; ++i)
            if (bins[i])
                std::fprintf(f, "%s,%d,%llu\n", msg_type, i,
                             (unsigned long long)bins[i]);
        std::fclose(f);
    }
};

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    std::string itch_file;
    std::string csv_dir;
    double mhz      = 250.0;
    size_t max_msgs = 0;
    bool   quiet    = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--csv"  && i + 1 < argc) csv_dir  = argv[++i];
        else if (a == "--mhz"  && i + 1 < argc) mhz      = std::atof(argv[++i]);
        else if (a == "--max"  && i + 1 < argc) max_msgs = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--quiet")                 quiet    = true;
        else if (a[0] != '-')                    itch_file = a;
    }

    if (itch_file.empty()) {
        std::fprintf(stderr, "Usage: tb_latency <itch_file> [--csv DIR] "
                             "[--mhz F] [--max N] [--quiet]\n");
        return 2;
    }
    if (mhz <= 0) { std::fprintf(stderr, "invalid --mhz\n"); return 2; }

    const double ns_per_cycle = 1000.0 / mhz;   // e.g. 4 ns at 250 MHz

    // ---- load + split -------------------------------------------------------
    std::vector<uint8_t> raw = read_itch_file(itch_file);
    if (raw.empty()) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", itch_file.c_str());
        return 2;
    }
    std::vector<RawMessage> msgs = split_messages(raw, max_msgs);
    std::vector<uint8_t>    stream = reserialize(msgs);

    // Per-message frame length in the stream = 2-byte length prefix + body,
    // so the driver can find each message's last ingested byte.
    std::vector<size_t> frame_len(msgs.size());
    for (size_t k = 0; k < msgs.size(); ++k)
        frame_len[k] = 2 + msgs[k].body.size();

    std::printf("tb_latency: %zu messages from %s  |  %.0f MHz (%.2f ns/cyc)\n",
                msgs.size(), itch_file.c_str(), mhz, ns_per_cycle);

    // ---- drive RTL ----------------------------------------------------------
    RtlDriver drv;
    drv.reset(/*raw_mode=*/true);

    std::array<Hist, TYPE_COUNT> hist_type;   // TRUE pipeline latency (ingest-excluded)
    Hist hist_all;
    Hist hist_service;                        // commit-to-commit = service time (throughput)

    uint64_t prev_commit_cyc = drv.cycles();

    // on_msg(idx, ingest_done_cyc, commit_cyc):
    //   pipeline latency = commit - ingest_done   (decode + book update + best)
    //   service time     = commit - prev commit   (1/throughput, ingest-bound)
    auto on_msg = [&](uint64_t idx, uint64_t ingest_cyc, uint64_t commit_cyc) {
        const double lat_ns = ingest_cyc
            ? static_cast<double>(commit_cyc - ingest_cyc) * ns_per_cycle : 0.0;
        const double svc_ns =
            static_cast<double>(commit_cyc - prev_commit_cyc) * ns_per_cycle;
        prev_commit_cyc = commit_cyc;

        const uint8_t tbyte = msgs[idx].body.empty() ? 0 : msgs[idx].body[0];
        const int tidx = type_byte_to_idx(tbyte);
        if (tidx >= 0) hist_type[tidx].record(lat_ns);
        hist_all.record(lat_ns);
        hist_service.record(svc_ns);

        if (!quiet && idx > 0 && (idx % 5000 == 0))
            std::printf("  [%8llu] total_cyc=%llu  latency_ns=%.1f  service_ns=%.1f\n",
                        (unsigned long long)idx,
                        (unsigned long long)drv.cycles(), lat_ns, svc_ns);
    };

    const uint64_t committed = drv.drive_latency(stream, frame_len, on_msg);
    const auto* top = drv.top();

    std::printf("tb_latency: committed %llu/%zu messages in %llu cycles\n",
                (unsigned long long)committed, msgs.size(),
                (unsigned long long)drv.cycles());

    if (committed != msgs.size()) {
        std::fprintf(stderr, "FAIL: pipeline hang — only %llu/%zu messages committed\n",
                     (unsigned long long)committed, msgs.size());
        return 1;
    }

    // ---- print histograms ---------------------------------------------------
    std::printf("\n==== RTL pipeline latency — ingest excluded (ns at %.0f MHz) ====\n", mhz);
    std::printf("     (cycles from a message's last input byte to its commit)\n");
    for (int i = 0; i < TYPE_COUNT; ++i)
        hist_type[i].print(TYPE_NAMES[i]);
    hist_all.print("ALL");
    std::printf("\n==== RTL service time — commit-to-commit = 1/throughput ====\n");
    hist_service.print("SERVICE");

    // ---- print RTL perf counters -------------------------------------------
    std::printf("\n==== RTL perf counters ====\n");
    std::printf("  add_cyc=%llu  del_cyc=%llu  repl_cyc=%llu\n"
                "  scans=%llu  scan_cyc_total=%llu\n"
                "  probe1=%llu  probe2=%llu  probe_gt2=%llu\n"
                "  msg_total=%llu  trade_count=%llu\n",
                (unsigned long long)top->add_cycles,
                (unsigned long long)top->delete_cycles,
                (unsigned long long)top->replace_cycles,
                (unsigned long long)top->scan_count,
                (unsigned long long)top->scan_cycles_total,
                (unsigned long long)top->hash_probe_1,
                (unsigned long long)top->hash_probe_2,
                (unsigned long long)top->hash_probe_gt2,
                (unsigned long long)top->msg_total,
                (unsigned long long)top->trade_count);
    if (top->best_bid_valid)
        std::printf("  best_bid=%u (depth=%u)\n",
                    (unsigned)top->best_bid_price, (unsigned)top->best_bid_shares);
    if (top->best_ask_valid)
        std::printf("  best_ask=%u (depth=%u)\n",
                    (unsigned)top->best_ask_price, (unsigned)top->best_ask_shares);
    if (top->vwap_den)
        std::printf("  vwap=$%.4f\n",
                    double(top->vwap_num) / double(top->vwap_den) / 10000.0);

    // ---- write CSVs --------------------------------------------------------
    if (!csv_dir.empty()) {
        for (int i = 0; i < TYPE_COUNT; ++i) {
            if (!hist_type[i].count) continue;
            std::string p = csv_dir + "/latency_hw_" + TYPE_NAMES[i] + ".csv";
            hist_type[i].write_csv(p.c_str(), TYPE_NAMES[i]);
        }
        {
            std::string p = csv_dir + "/latency_hw_all.csv";
            hist_all.write_csv(p.c_str(), "ALL");
        }

        std::string pc = csv_dir + "/perf_counters_hw.csv";
        if (FILE* f = std::fopen(pc.c_str(), "w")) {
            const double svc_p50 = hist_service.percentile(0.50);
            const double thru_msg_s = (svc_p50 > 0) ? 1e9 / svc_p50 : 0.0;
            std::fprintf(f, "counter,value\n");
            std::fprintf(f, "messages,%llu\n",          (unsigned long long)committed);
            std::fprintf(f, "total_cycles,%llu\n",      (unsigned long long)drv.cycles());
            std::fprintf(f, "mhz,%.0f\n",               mhz);
            // p50/p99_all_ns are now TRUE pipeline latency (ingest excluded).
            std::fprintf(f, "p50_all_ns,%.0f\n",        hist_all.percentile(0.50));
            std::fprintf(f, "p99_all_ns,%.0f\n",        hist_all.percentile(0.99));
            std::fprintf(f, "p9999_all_ns,%.0f\n",      hist_all.percentile(0.9999));
            std::fprintf(f, "service_p50_ns,%.0f\n",    svc_p50);
            std::fprintf(f, "throughput_msg_s,%.0f\n",  thru_msg_s);
            std::fprintf(f, "add_cycles,%llu\n",         (unsigned long long)top->add_cycles);
            std::fprintf(f, "delete_cycles,%llu\n",      (unsigned long long)top->delete_cycles);
            std::fprintf(f, "replace_cycles,%llu\n",     (unsigned long long)top->replace_cycles);
            std::fprintf(f, "scan_count,%llu\n",         (unsigned long long)top->scan_count);
            std::fprintf(f, "scan_cycles_total,%llu\n",  (unsigned long long)top->scan_cycles_total);
            std::fprintf(f, "hash_probe_1,%llu\n",       (unsigned long long)top->hash_probe_1);
            std::fprintf(f, "hash_probe_2,%llu\n",       (unsigned long long)top->hash_probe_2);
            std::fprintf(f, "hash_probe_gt2,%llu\n",     (unsigned long long)top->hash_probe_gt2);
            std::fprintf(f, "msg_total,%llu\n",          (unsigned long long)top->msg_total);
            std::fprintf(f, "trade_count,%llu\n",        (unsigned long long)top->trade_count);
            std::fclose(f);
        }

        std::printf("\n[csv] wrote latency_hw_*.csv + perf_counters_hw.csv to %s/\n",
                    csv_dir.c_str());
    }
    return 0;
}
