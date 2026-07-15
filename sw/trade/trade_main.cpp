// orderbook_trade — DSL strategies against the replayed ITCH book with
// queue-aware fills. The counterpart to backtest_project's analytic fills:
// same strategy bytecode, real order mechanics.
//
//   build/orderbook_trade --gen 500000 --aggress 20 --ts-scale 100000
//       --strategy data/strategies/sma_cross.obp --bar-secs 5 --csv out/
//   (one command line; wrapped here for width)
//
// Feed: --gen N (synthetic, deterministic by --seed) or --file stream.itch.
// Entry style: --entry join (passive, default) or cross. CSVs (bars, fills,
// trades, equity) land in --csv DIR for tools/compare_backtest.py.
#include "session.hpp"
#include "../util/itch_gen.hpp"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace ob;
using namespace ob::trade;

namespace {

struct Args {
    std::string strategy;
    std::string file;
    size_t gen = 0;
    uint64_t seed = 0xC0FFEE12345ULL;
    uint32_t aggress = 20;
    uint64_t ts_scale = 100000;
    uint16_t symbol = 1;
    double bar_secs = 5.0;
    std::string entry = "join";
    float fee = 0.001f;
    float equity0 = 1e9f;
    std::string csv_dir;
    std::vector<std::pair<std::string, float>> params;
};

[[noreturn]] void usage(const char* argv0) {
    std::printf(
        "usage: %s --strategy S.obp [--gen N | --file F.itch] [options]\n"
        "  --symbol L      stock locate to trade (default 1)\n"
        "  --bar-secs S    bar interval in seconds (default 5)\n"
        "  --entry MODE    join | cross (default join)\n"
        "  --fee F         fee fraction per fill notional (default 0.001)\n"
        "  --equity0 E     starting cash, raw price units (default 1e9)\n"
        "  --param k=v     override a strategy param (repeatable)\n"
        "  --csv DIR       write bars/fills/trades/equity CSVs\n"
        "  --seed X --aggress P --ts-scale K   generator knobs (with --gen)\n",
        argv0);
    std::exit(2);
}

Args parse_args(int argc, char** argv) {
    Args a;
    auto need = [&](int& i) -> const char* {
        if (++i >= argc) usage(argv[0]);
        return argv[i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--strategy") a.strategy = need(i);
        else if (k == "--file") a.file = need(i);
        else if (k == "--gen") a.gen = std::strtoull(need(i), nullptr, 10);
        else if (k == "--seed") a.seed = std::strtoull(need(i), nullptr, 0);
        else if (k == "--aggress") a.aggress = (uint32_t)std::strtoul(need(i), nullptr, 10);
        else if (k == "--ts-scale") a.ts_scale = std::strtoull(need(i), nullptr, 10);
        else if (k == "--symbol") a.symbol = (uint16_t)std::strtoul(need(i), nullptr, 10);
        else if (k == "--bar-secs") a.bar_secs = std::strtod(need(i), nullptr);
        else if (k == "--entry") a.entry = need(i);
        else if (k == "--fee") a.fee = std::strtof(need(i), nullptr);
        else if (k == "--equity0") a.equity0 = std::strtof(need(i), nullptr);
        else if (k == "--csv") a.csv_dir = need(i);
        else if (k == "--param") {
            std::string kv = need(i);
            size_t eq = kv.find('=');
            if (eq == std::string::npos) usage(argv[0]);
            a.params.emplace_back(kv.substr(0, eq),
                                  std::strtof(kv.c_str() + eq + 1, nullptr));
        } else usage(argv[0]);
    }
    if (a.strategy.empty() || (a.gen == 0 && a.file.empty())) usage(argv[0]);
    if (a.entry != "join" && a.entry != "cross") usage(argv[0]);
    if (!a.file.empty() && a.gen != 0) usage(argv[0]);
    if (!std::isfinite(a.bar_secs) || a.bar_secs < 1e-9 ||
        a.bar_secs > double(UINT64_MAX) / 1e9 ||
        !std::isfinite(a.fee) || a.fee < 0.0f ||
        !std::isfinite(a.equity0) || a.equity0 <= 0.0f ||
        a.symbol == 0 || a.aggress > 100 || a.ts_scale == 0)
        usage(argv[0]);
    for (const auto& param : a.params)
        if (!std::isfinite(param.second)) usage(argv[0]);
    return a;
}

std::vector<uint8_t> load_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("cannot open %s\n", path.c_str()); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz > 0 ? size_t(sz) : 0);
    size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) { std::printf("short read on %s\n", path.c_str()); std::exit(1); }
    return buf;
}

void write_csvs(const std::string& dir, const SessionResult& r) {
    std::filesystem::create_directories(dir);
    auto open = [&](const char* name) {
        std::string p = dir + "/" + name;
        std::FILE* f = std::fopen(p.c_str(), "wb");
        if (!f) { std::printf("cannot write %s\n", p.c_str()); std::exit(1); }
        return f;
    };
    std::FILE* f = open("bars.csv");
    std::fprintf(f, "t,o,h,l,c,v\n");
    for (int t = 0; t < r.bars.size(); ++t)
        std::fprintf(f, "%d,%.9g,%.9g,%.9g,%.9g,%.9g\n", t, r.bars.o[t],
                     r.bars.h[t], r.bars.l[t], r.bars.c[t], r.bars.v[t]);
    std::fclose(f);

    f = open("fills.csv");
    std::fprintf(f, "ts,our_ref,side,price,shares,maker\n");
    for (const FillRec& x : r.fills)
        std::fprintf(f, "%llu,%llu,%c,%u,%u,%d\n",
                     (unsigned long long)x.ts, (unsigned long long)x.our_ref,
                     x.side, x.price, x.shares, x.maker ? 1 : 0);
    std::fclose(f);

    f = open("trades.csv");
    std::fprintf(f, "side,entry_t,exit_t,entry_px,exit_px,qty,pnl,reason\n");
    for (const TradeRecT& x : r.trades)
        std::fprintf(f, "%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%s\n", x.side, x.entry_t,
                     x.exit_t, x.entry_px, x.exit_px, x.qty, x.pnl,
                     reason_name(x.reason));
    std::fclose(f);

    f = open("equity.csv");
    std::fprintf(f, "t,equity\n");
    for (size_t t = 0; t < r.equity.size(); ++t)
        std::fprintf(f, "%zu,%.9g\n", t, r.equity[t]);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    Program prog = Program::load(a.strategy);
    std::vector<float> params = prog.param_defaults;
    for (const auto& [name, val] : a.params) {
        bool found = false;
        for (size_t i = 0; i < prog.param_names.size(); ++i)
            if (prog.param_names[i] == name) { params[i] = val; found = true; }
        if (!found) {
            std::printf("unknown param '%s' (strategy has:", name.c_str());
            for (const auto& n : prog.param_names) std::printf(" %s", n.c_str());
            std::printf(")\n");
            return 1;
        }
    }

    std::vector<uint8_t> bytes;
    if (a.gen > 0) {
        GenConfig gc;
        gc.seed = a.seed;
        gc.aggress_prob = a.aggress;
        gc.ts_scale = a.ts_scale;
        bytes = ItchGenerator(gc).generate(a.gen);
    } else {
        bytes = load_file(a.file);
    }

    SessionConfig sc;
    sc.locate = a.symbol;
    sc.bar_ns = (uint64_t)(a.bar_secs * 1e9);
    sc.om.fee = a.fee;
    sc.om.equity0 = a.equity0;
    sc.om.entry_cross = (a.entry == "cross");
    sc.params = params;

    SessionResult r = run_session(bytes.data(), bytes.size(), prog, sc);

    std::printf("strategy   %s (%s entries, fee %.4f)\n", a.strategy.c_str(),
                a.entry.c_str(), sc.om.fee);
    std::printf("stream     %llu messages -> %d bars of %.3gs\n",
                (unsigned long long)r.messages, r.bars.size(), a.bar_secs);
    const auto& bc = r.book_counters;
    std::printf("integrity  malformed=%llu truncated=%d invalid=%llu duplicate=%llu "
                "missing=%llu table_full=%llu resync=%llu\n",
                (unsigned long long)r.malformed_messages,
                r.truncated_stream ? 1 : 0,
                (unsigned long long)bc.invalid,
                (unsigned long long)bc.duplicate_ref,
                (unsigned long long)bc.missing_ref,
                (unsigned long long)bc.ref_insert_fail,
                (unsigned long long)bc.resync_required);
    if (r.malformed_messages || r.truncated_stream || bc.invalid ||
        bc.duplicate_ref || bc.missing_ref || bc.ref_insert_fail ||
        bc.resync_required) {
        std::fprintf(stderr,
                     "[error] session replay was incomplete; trading results and "
                     "CSV outputs were suppressed because they must not be trusted\n");
        return 3;
    }
    std::printf("fills      %d maker / %d taker\n", r.maker_fills, r.taker_fills);
    std::printf("trades     %zu (%d wins, %.1f%% win rate)\n", r.trades.size(),
                r.wins, r.trades.empty() ? 0.0 : 100.0 * r.wins / r.trades.size());
    std::printf("equity     %.2f -> %.2f  (return %+.4f%%, max dd %.4f%%)\n",
                (double)sc.om.equity0, (double)r.final_equity,
                (double)r.total_return * 100.0, (double)r.max_dd * 100.0);
    for (const TradeRecT& t : r.trades)
        std::printf("  trade %+d qty %g  %g -> %g  bars %d..%d  pnl %+.1f (%s)\n",
                    t.side, (double)t.qty, (double)t.entry_px, (double)t.exit_px,
                    t.entry_t, t.exit_t, (double)t.pnl, reason_name(t.reason));

    if (!a.csv_dir.empty()) {
        write_csvs(a.csv_dir, r);
        std::printf("[csv] wrote bars/fills/trades/equity to %s/\n", a.csv_dir.c_str());
    }
    return 0;
}
