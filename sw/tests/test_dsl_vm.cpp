// StrategyVM golden-parity tests: replay the per-bar context recorded from
// backtest_project's refengine.py (tools/gen_vm_golden.py) and require the
// C++ VM to reproduce every signal flag exactly and every config/local to
// 1e-6 relative (absorbs libm ulp differences in log; everything else is
// bit-identical float32 by construction).
#include "test_harness.hpp"
#include "../trade/dsl_vm.hpp"
#include "../trade/program.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ob::trade;

namespace {

std::vector<std::vector<float>> read_csv(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::vector<std::vector<float>> rows;
    char line[2048];
    bool header = true;
    while (std::fgets(line, sizeof line, f)) {
        if (header) { header = false; continue; }
        std::vector<float> row;
        char* p = line;
        while (*p) {
            row.push_back(std::strtof(p, &p));
            if (*p == ',') ++p; else break;
        }
        if (!row.empty()) rows.push_back(std::move(row));
    }
    std::fclose(f);
    return rows;
}

BarSeries load_bars() {
    BarSeries bars;
    for (const auto& r : read_csv("data/strategies/golden/bars.csv")) {
        bars.o.push_back(r[1]);
        bars.h.push_back(r[2]);
        bars.l.push_back(r[3]);
        bars.c.push_back(r[4]);
        bars.v.push_back(r[5]);
    }
    return bars;
}

bool close_rel(float a, float b) {
    float d = std::fabs(a - b);
    float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return d <= 1e-6f * (m > 1.0f ? m : 1.0f);
}

// Replays one strategy against its golden file; returns mismatch count so a
// broken opcode fails loudly rather than drowning the log per-bar.
void golden_replay(const char* name) {
    BarSeries bars = load_bars();
    CHECK_EQ(bars.size(), 600);

    Program prog = Program::load(std::string("data/strategies/") + name + ".obp");
    auto rows = read_csv(std::string("data/strategies/golden/") + name + "_golden.csv");
    CHECK_EQ((int)rows.size(), bars.size());

    StrategyVM vm(prog);
    int bad = 0;
    for (int t = 0; t < (int)rows.size(); ++t) {
        const auto& r = rows[t];
        // columns: t,position,entry_px,equity,el,xl,es,xs,stop,tp,trail,size,locals...
        VmCtx ctx{r[1], r[2], r[3]};
        Signals s = vm.step(bars, t, ctx);
        bool row_ok =
            s.el == (r[4] != 0.0f) && s.xl == (r[5] != 0.0f) &&
            s.es == (r[6] != 0.0f) && s.xs == (r[7] != 0.0f) &&
            close_rel(s.stop, r[8]) && close_rel(s.tp, r[9]) &&
            close_rel(s.trail, r[10]) && close_rel(s.size, r[11]);
        size_t n_locals = r.size() - 12;
        CHECK(n_locals <= vm.locals().size());
        for (size_t i = 0; i < n_locals; ++i)
            if (!close_rel(vm.locals()[i], r[12 + i])) row_ok = false;
        if (!row_ok && ++bad <= 3)
            std::printf("    [mismatch] %s bar %d\n", name, t);
    }
    CHECK_EQ(bad, 0);
}

}  // namespace

static void test_golden_sma_cross()    { golden_replay("sma_cross"); }
static void test_golden_rsi_meanrev()  { golden_replay("rsi_meanrev"); }
static void test_golden_breakout()     { golden_replay("breakout"); }
static void test_golden_kitchen_sink() { golden_replay("kitchen_sink"); }

// Same program + same input twice -> identical outputs (state is per-VM).
static void test_vm_deterministic() {
    BarSeries bars = load_bars();
    Program prog = Program::load("data/strategies/kitchen_sink.obp");
    StrategyVM a(prog), b2(prog);
    VmCtx ctx{0.0f, 0.0f, 10000.0f};
    for (int t = 0; t < 50; ++t) {
        Signals sa = a.step(bars, t, ctx);
        Signals sb = b2.step(bars, t, ctx);
        CHECK(sa.el == sb.el && sa.xl == sb.xl && sa.es == sb.es && sa.xs == sb.xs);
        CHECK(sa.stop == sb.stop && sa.tp == sb.tp &&
              sa.trail == sb.trail && sa.size == sb.size);
    }
    CHECK_EQ((int)std::memcmp(a.locals().data(), b2.locals().data(),
                              a.locals().size() * sizeof(float)), 0);
}

static void test_param_override() {
    Program prog = Program::load("data/strategies/sma_cross.obp");
    // wrong param count throws
    bool threw = false;
    try {
        StrategyVM bad(prog, {1.0f});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    // explicit defaults behave like the no-param constructor
    BarSeries bars = load_bars();
    StrategyVM va(prog), vb(prog, prog.param_defaults);
    VmCtx ctx{};
    for (int t = 0; t < 200; ++t) {
        Signals sa = va.step(bars, t, ctx);
        Signals sb = vb.step(bars, t, ctx);
        CHECK(sa.el == sb.el && sa.xl == sb.xl);
    }
}

void run_dsl_vm_tests() {
    RUN_TEST(test_golden_sma_cross);
    RUN_TEST(test_golden_rsi_meanrev);
    RUN_TEST(test_golden_breakout);
    RUN_TEST(test_golden_kitchen_sink);
    RUN_TEST(test_vm_deterministic);
    RUN_TEST(test_param_override);
}
