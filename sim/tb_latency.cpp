// tb_latency.cpp — RTL per-message latency measurement via Verilator.
//
// Replays a project-format ITCH file through Verilated orderbook_top and
// records cycle counts directly. A user-set clock is metadata used only for
// throughput conversion. Emits:
//   latency_hw_<Type>.csv  — pipeline-latency cycle histograms
//   latency_hw_all.csv     — aggregated histogram
//   latency_hw_service.csv — commit-to-commit cycle histogram
//   perf_counters_hw.csv   — RTL hardware perf counter final values
//
// Usage:
//   tb_latency <itch_file> [--csv DIR] [--mhz F] [--max N] [--quiet]
//
//   --mhz F  Implemented clock for throughput conversion (default 100 MHz).
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
#include <map>

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

// ---- exact sparse cycle histogram (no clipping/overflow bucket) ------------

struct Hist {
    std::map<uint64_t, uint64_t> bins;
    uint64_t count = 0;
    uint64_t sum_cycles = 0;
    uint64_t max_cycles = 0;

    void record(uint64_t cycles) {
        ++bins[cycles];
        ++count;
        sum_cycles += cycles;
        if (cycles > max_cycles) max_cycles = cycles;
    }

    uint64_t percentile(double p) const {
        if (!count) return 0;
        uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(count)));
        uint64_t cum = 0;
        for (const auto& [cycles, n] : bins) {
            cum += n;
            if (cum >= target) return cycles;
        }
        return bins.empty() ? 0 : bins.rbegin()->first;
    }

    void print(const char* label) const {
        if (!count) return;
        std::printf(
            "  %-22s n=%-9llu  p50=%6llu  p95=%6llu  p99=%6llu  p99.9=%6llu  "
            "p99.99=%6llu  max=%7llu  mean=%6.1f  (cycles)\n",
            label, (unsigned long long)count,
            (unsigned long long)percentile(0.50),
            (unsigned long long)percentile(0.95),
            (unsigned long long)percentile(0.99),
            (unsigned long long)percentile(0.999),
            (unsigned long long)percentile(0.9999),
            (unsigned long long)max_cycles,
            count ? double(sum_cycles) / double(count) : 0.0);
    }

    void write_csv(const char* path, const char* msg_type) const {
        FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::fprintf(f, "msg_type,latency_cycles,count\n");
        for (const auto& [cycles, n] : bins)
            std::fprintf(f, "%s,%llu,%llu\n", msg_type,
                         (unsigned long long)cycles, (unsigned long long)n);
        std::fclose(f);
    }
};

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    std::string itch_file;
    std::string csv_dir;
    double mhz      = 100.0;
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

    std::printf("tb_latency: %zu messages from %s  |  implemented clock %.0f MHz\n",
                msgs.size(), itch_file.c_str(), mhz);

    // ---- drive RTL ----------------------------------------------------------
    RtlDriver drv;
    drv.reset(/*raw_mode=*/true);

    std::array<Hist, TYPE_COUNT> hist_type;   // pipeline latency (ingest-excluded)
    Hist hist_all;
    Hist hist_service;                        // commit-to-commit = service time (throughput)
    std::array<Hist, TYPE_COUNT> hist_e2e_type;  // first-byte-to-commit (end-to-end)
    Hist hist_e2e_all;

    uint64_t prev_commit_cyc = drv.cycles();

    // on_msg(idx, ingest_first_cyc, ingest_done_cyc, commit_cyc):
    //   pipeline latency = commit - ingest_done   (decode + book update + best;
    //                      with overlapped ingest this includes any wait for
    //                      the engine to finish the previous message)
    //   end-to-end       = commit - ingest_first  (first input byte to commit)
    //   service time     = commit - prev commit   (1/throughput)
    auto on_msg = [&](uint64_t idx, uint64_t first_cyc, uint64_t ingest_cyc,
                      uint64_t commit_cyc) {
        const uint64_t lat_cycles = ingest_cyc ? commit_cyc - ingest_cyc : 0;
        const uint64_t e2e_cycles = first_cyc ? commit_cyc - first_cyc : 0;
        const uint64_t svc_cycles = commit_cyc - prev_commit_cyc;
        prev_commit_cyc = commit_cyc;

        const uint8_t tbyte = msgs[idx].body.empty() ? 0 : msgs[idx].body[0];
        const int tidx = type_byte_to_idx(tbyte);
        if (tidx >= 0) {
            hist_type[tidx].record(lat_cycles);
            hist_e2e_type[tidx].record(e2e_cycles);
        }
        hist_all.record(lat_cycles);
        hist_e2e_all.record(e2e_cycles);
        hist_service.record(svc_cycles);

        if (!quiet && idx > 0 && (idx % 5000 == 0))
            std::printf("  [%8llu] total=%llu  pipeline=%llu  e2e=%llu  service=%llu cycles\n",
                        (unsigned long long)idx,
                        (unsigned long long)drv.cycles(),
                        (unsigned long long)lat_cycles,
                        (unsigned long long)e2e_cycles,
                        (unsigned long long)svc_cycles);
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
    std::printf("\n==== RTL pipeline latency — ingest excluded (cycles) ====\n");
    std::printf("     (cycles from a message's last input byte to its commit;\n"
                "      ingest overlaps the previous message, so this includes\n"
                "      any wait for the engine to free up)\n");
    for (int i = 0; i < TYPE_COUNT; ++i)
        hist_type[i].print(TYPE_NAMES[i]);
    hist_all.print("ALL");
    std::printf("\n==== RTL end-to-end latency — first input byte to commit ====\n");
    for (int i = 0; i < TYPE_COUNT; ++i)
        hist_e2e_type[i].print(TYPE_NAMES[i]);
    hist_e2e_all.print("ALL");
    std::printf("\n==== RTL service time — commit-to-commit = 1/throughput ====\n");
    hist_service.print("SERVICE");

    // ---- print RTL perf counters -------------------------------------------
    std::printf("\n==== RTL perf counters ====\n");
    std::printf("  ingest_stall_cyc=%llu\n",
                (unsigned long long)top->ingest_stall_cycles);
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
        for (int i = 0; i < TYPE_COUNT; ++i) {
            if (!hist_e2e_type[i].count) continue;
            std::string p = csv_dir + "/latency_hw_e2e_" + TYPE_NAMES[i] + ".csv";
            hist_e2e_type[i].write_csv(p.c_str(), TYPE_NAMES[i]);
        }
        {
            std::string p = csv_dir + "/latency_hw_e2e_all.csv";
            hist_e2e_all.write_csv(p.c_str(), "ALL");
        }
        {
            std::string p = csv_dir + "/latency_hw_service.csv";
            hist_service.write_csv(p.c_str(), "SERVICE");
        }

        std::string pc = csv_dir + "/perf_counters_hw.csv";
        if (FILE* f = std::fopen(pc.c_str(), "w")) {
            const uint64_t svc_p50 = hist_service.percentile(0.50);
            const double thru_msg_s = svc_p50 ? mhz * 1e6 / double(svc_p50) : 0.0;
            std::fprintf(f, "counter,value\n");
            std::fprintf(f, "messages,%llu\n",          (unsigned long long)committed);
            std::fprintf(f, "total_cycles,%llu\n",      (unsigned long long)drv.cycles());
            std::fprintf(f, "clock_mhz,%.0f\n",         mhz);
            std::fprintf(f, "pipeline_p50_cycles,%llu\n", (unsigned long long)hist_all.percentile(0.50));
            std::fprintf(f, "pipeline_p99_cycles,%llu\n", (unsigned long long)hist_all.percentile(0.99));
            std::fprintf(f, "pipeline_p999_cycles,%llu\n", (unsigned long long)hist_all.percentile(0.999));
            std::fprintf(f, "e2e_p50_cycles,%llu\n",    (unsigned long long)hist_e2e_all.percentile(0.50));
            std::fprintf(f, "e2e_p99_cycles,%llu\n",    (unsigned long long)hist_e2e_all.percentile(0.99));
            std::fprintf(f, "e2e_p999_cycles,%llu\n",   (unsigned long long)hist_e2e_all.percentile(0.999));
            std::fprintf(f, "service_p50_cycles,%llu\n", (unsigned long long)svc_p50);
            std::fprintf(f, "service_p99_cycles,%llu\n", (unsigned long long)hist_service.percentile(0.99));
            std::fprintf(f, "service_p999_cycles,%llu\n", (unsigned long long)hist_service.percentile(0.999));
            std::fprintf(f, "throughput_msg_s,%.0f\n",  thru_msg_s);
            std::fprintf(f, "add_cycles,%llu\n",         (unsigned long long)top->add_cycles);
            std::fprintf(f, "delete_cycles,%llu\n",      (unsigned long long)top->delete_cycles);
            std::fprintf(f, "replace_cycles,%llu\n",     (unsigned long long)top->replace_cycles);
            std::fprintf(f, "scan_count,%llu\n",         (unsigned long long)top->scan_count);
            std::fprintf(f, "scan_cycles_total,%llu\n",  (unsigned long long)top->scan_cycles_total);
            std::fprintf(f, "hash_probe_1,%llu\n",       (unsigned long long)top->hash_probe_1);
            std::fprintf(f, "hash_probe_2,%llu\n",       (unsigned long long)top->hash_probe_2);
            std::fprintf(f, "hash_probe_gt2,%llu\n",     (unsigned long long)top->hash_probe_gt2);
            std::fprintf(f, "ingest_stall_cycles,%llu\n",(unsigned long long)top->ingest_stall_cycles);
            std::fprintf(f, "msg_total,%llu\n",          (unsigned long long)top->msg_total);
            std::fprintf(f, "trade_count,%llu\n",        (unsigned long long)top->trade_count);
            std::fclose(f);
        }

        std::printf("\n[csv] wrote latency_hw_*.csv + perf_counters_hw.csv to %s/\n",
                    csv_dir.c_str());
    }
    return 0;
}
