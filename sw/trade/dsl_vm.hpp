// Per-bar stepping VM for OBP1 strategy bytecode.
//
// A faithful port of the opcode interpreter in backtest_project's
// engine/vm_core.h (which in turn mirrors dsl/refengine.py operation-for-
// operation in float32) — minus the fused broker: instead of filling orders
// itself, step() returns the bar's signal flags and config sinks and the
// caller decides what to do with them. Indicator state persists across
// step() calls exactly like one batch-engine thread's state block.
//
// Float32 discipline: every intermediate is a float, in the same operation
// order as the reference. This translation unit is compiled with
// -ffp-contract=off (see Makefile) so g++ cannot fuse a*b+c into fma and
// break bit-parity with the golden model. Validated against refengine.py by
// sw/tests/test_dsl_vm.cpp's golden-file tests.
#pragma once

#include <vector>

#include "bar_builder.hpp"
#include "program.hpp"

namespace ob::trade {

// One bar's program outputs: latched signal flags plus the config sinks
// (defaults re-established every bar; an omitted `set` keeps the default).
struct Signals {
    bool el = false, xl = false, es = false, xs = false;
    float stop = 0.0f, tp = 0.0f, trail = 0.0f, size = 1.0f;
};

// Context scalars the strategy can read (PUSH_CTX); supplied by the caller
// per bar. Mirrors vm_core: position is -1/0/+1, entry_px 0 when flat,
// equity marked to the bar close.
struct VmCtx {
    float position = 0.0f;
    float entry_px = 0.0f;
    float equity = 0.0f;
};

class StrategyVM {
public:
    // Empty params = program defaults. Throws std::runtime_error on a param
    // count mismatch.
    explicit StrategyVM(const Program& prog, std::vector<float> params = {});

    // Run the program once for completed bar t (requires t < bars.size()).
    Signals step(const BarSeries& bars, int t, const VmCtx& ctx);

    const std::vector<float>& locals() const { return locals_; }

private:
    const Program& prog_;
    std::vector<float> params_;
    std::vector<float> state_;   // indicator state, persists across bars
    std::vector<float> locals_;
};

}  // namespace ob::trade
