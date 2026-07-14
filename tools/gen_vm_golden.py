"""Generate golden CSVs for the C++ StrategyVM parity tests.

Runs each checked-in strategy through an *instrumented copy* of
backtest_project's reference engine (dsl/refengine.py) on deterministic
synthetic bars, recording per bar everything sw/tests/test_dsl_vm.cpp needs:
the context inputs the program saw (position, entry_px, equity) and the
outputs it produced (signal flags, config sinks, locals).

The copy is kept honest by a cross-check: its equity curve, trades, and
locals must match dsl.refengine.run() exactly on the same input, or this
script aborts. So the goldens are refengine's semantics by construction.

Usage (from orderbook_project/):
    python tools/gen_vm_golden.py [--backtest ../backtest_project]

Outputs: data/strategies/golden/bars.csv and <strategy>_golden.csv.
Regenerate only when the DSL/VM semantics or the strategies change, then
re-run `make test` and commit the new goldens alongside.
"""

from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

parser = argparse.ArgumentParser()
parser.add_argument("--backtest", default=os.path.join(PROJ, "..", "backtest_project"))
args = parser.parse_args()
sys.path.insert(0, os.path.abspath(args.backtest))

import numpy as np  # noqa: E402

from dsl import refengine  # noqa: E402
from dsl.compiler import Program, compile_source  # noqa: E402
from dsl.export import export_file  # noqa: E402
from dsl.opcodes import OP  # noqa: E402

F = np.float32
F0 = F(0.0)
F1 = F(1.0)
_OPN = {v: k for k, v in OP.items()}

STRATEGIES = ["sma_cross", "rsi_meanrev", "breakout", "kitchen_sink"]
T = 600
SEED = 20260712


class Bars:
    """Attribute-style OHLCV arrays (what refengine.run expects)."""

    def __init__(self, o, h, l, c, v):
        self.open, self.high, self.low, self.close, self.volume = o, h, l, c, v


def make_bars() -> Bars:
    """Deterministic float32 random walk in raw ITCH price units (1e-4 USD)."""
    rng = np.random.default_rng(SEED)
    steps = rng.normal(0.0, 0.004, T)
    c = (1_000_000.0 * np.exp(np.cumsum(steps))).astype(np.float32)
    o = np.empty(T, np.float32)
    o[0] = F(1_000_000.0)
    o[1:] = c[:-1]
    spread = np.abs(rng.normal(0.0, 0.002, T))
    h = (np.maximum(o, c) * (1.0 + spread)).astype(np.float32)
    l = (np.minimum(o, c) * (1.0 - spread)).astype(np.float32)
    v = rng.integers(100, 10_000, T).astype(np.float32)
    return Bars(o, h, l, c, v)


# ---------------------------------------------------------------------------
# Instrumented reference run: identical to refengine.run except it records the
# per-bar program context and outputs. Broker/metrics parts are delegated to
# refengine's own helpers where possible; the interpreter loop is a verbatim
# copy with recording hooks (cross-checked below).
# ---------------------------------------------------------------------------

def instrumented_run(program: Program, bars, cfg: refengine.BacktestConfig):
    pvals = [F(v) for v in program.param_defaults()]
    ops = [(int(cw) >> 24, int(cw) & 0xFFFFFF) for cw in program.code]
    consts = [F(cv) for cv in program.consts]
    state = np.zeros(program.state_floats, dtype=np.float32)
    locals_ = np.zeros(max(program.n_locals, 1), dtype=np.float32)
    scap, soff, saux = program.state_cap, program.state_off, program.state_aux

    o, h, l, c, v = bars.open, bars.high, bars.low, bars.close, bars.volume
    n_bars = len(c)
    fee, slip = F(cfg.fee_rate), F(cfg.slip)

    cash = F(cfg.equity0)
    qty, side, entry_px, entry_t, anchor = F0, 0, F0, -1, F0
    pend_action, pend_size = 0, F1
    cfg_stop = cfg_tp = cfg_trail = F0
    cfg_size = F1

    equity_curve = np.zeros(n_bars, dtype=np.float32)
    trades = []
    rows = []  # per-bar golden rows

    def qsigned():
        return qty if side >= 0 else F(-qty)

    def open_pos(t, direction, px):
        nonlocal cash, qty, side, entry_px, entry_t, anchor
        eq = cash
        q = F(eq * pend_size / px) if px > F0 else F0
        if q <= F0:
            return
        cash = F(cash - F(direction) * q * px)
        cash = F(cash - q * px * fee)
        qty, side, entry_px, entry_t, anchor = q, direction, px, t, px

    def close_pos(t, px, reason):
        nonlocal cash, qty, side, entry_px, entry_t, anchor
        cash = F(cash + F(side) * qty * px)
        fee_exit = F(qty * px * fee)
        cash = F(cash - fee_exit)
        gross = F(F(side) * qty * F(px - entry_px))
        fee_entry = F(qty * entry_px * fee)
        trades.append(refengine.Trade(side, entry_t, t, float(entry_px), float(px),
                                      float(qty), float(F(gross - F(fee_entry + fee_exit))),
                                      reason))
        qty, side, entry_px, entry_t, anchor = F0, 0, F0, -1, F0

    for t in range(n_bars):
        if pend_action == 2 and side != 0:
            px = F(o[t] * F(F1 - slip)) if side > 0 else F(o[t] * F(F1 + slip))
            close_pos(t, px, "signal")
        elif pend_action in (1, -1) and side == 0:
            px = F(o[t] * F(F1 + slip)) if pend_action > 0 else F(o[t] * F(F1 - slip))
            open_pos(t, pend_action, px)
        pend_action = 0

        if side > 0:
            if cfg_stop > F0:
                trig = F(entry_px * F(F1 - cfg_stop))
                if l[t] <= trig:
                    close_pos(t, F(min(o[t], trig) * F(F1 - slip)), "stop")
            if side > 0 and cfg_trail > F0:
                trig = F(anchor * F(F1 - cfg_trail))
                if l[t] <= trig:
                    close_pos(t, F(min(o[t], trig) * F(F1 - slip)), "trail")
            if side > 0 and cfg_tp > F0:
                trig = F(entry_px * F(F1 + cfg_tp))
                if h[t] >= trig:
                    close_pos(t, F(max(o[t], trig) * F(F1 - slip)), "tp")
        elif side < 0:
            if cfg_stop > F0:
                trig = F(entry_px * F(F1 + cfg_stop))
                if h[t] >= trig:
                    close_pos(t, F(max(o[t], trig) * F(F1 + slip)), "stop")
            if side < 0 and cfg_trail > F0:
                trig = F(anchor * F(F1 + cfg_trail))
                if h[t] >= trig:
                    close_pos(t, F(max(o[t], trig) * F(F1 + slip)), "trail")
            if side < 0 and cfg_tp > F0:
                trig = F(entry_px * F(F1 - cfg_tp))
                if l[t] <= trig:
                    close_pos(t, F(min(o[t], trig) * F(F1 + slip)), "tp")

        # ---- run the bar program (recorded ctx inputs) --------------------
        eq_now = F(cash + qsigned() * c[t])
        ctx_position, ctx_entry_px = side, entry_px

        stack = []
        push, pop = stack.append, stack.pop
        sig_el = sig_xl = sig_es = sig_xs = False
        cfg_stop, cfg_tp, cfg_trail, cfg_size = F0, F0, F0, F1

        for opcode, arg in ops:
            name = _OPN[opcode]
            if name == "PUSH_CONST":
                push(consts[arg])
            elif name == "PUSH_PARAM":
                push(pvals[arg])
            elif name == "PUSH_SERIES":
                push((o, h, l, c, v)[arg][t])
            elif name == "PUSH_SERIES_LAG":
                sid, lag = arg >> 16, arg & 0xFFFF
                push((o, h, l, c, v)[sid][max(t - lag, 0)])
            elif name == "PUSH_CTX":
                push((F(t), F(side), entry_px, eq_now)[arg])
            elif name == "LOAD":
                push(locals_[arg])
            elif name == "STORE":
                locals_[arg] = pop()
            elif name == "ADD":
                b = pop(); push(F(pop() + b))
            elif name == "SUB":
                b = pop(); push(F(pop() - b))
            elif name == "MUL":
                b = pop(); push(F(pop() * b))
            elif name == "DIV":
                b = pop(); a = pop()
                push(F(a / b) if b != F0 else F0)
            elif name == "NEG":
                push(F(-pop()))
            elif name == "ABS":
                push(F(abs(pop())))
            elif name == "MIN2":
                b = pop(); push(F(min(pop(), b)))
            elif name == "MAX2":
                b = pop(); push(F(max(pop(), b)))
            elif name == "SQRT":
                a = pop()
                push(F(np.sqrt(a)) if a > F0 else F0)
            elif name == "LOG":
                a = pop()
                push(F(np.log(a)) if a > F0 else F0)
            elif name in ("GT", "LT", "GE", "LE", "EQ", "NE"):
                b = pop(); a = pop()
                r = {"GT": a > b, "LT": a < b, "GE": a >= b,
                     "LE": a <= b, "EQ": a == b, "NE": a != b}[name]
                push(F1 if r else F0)
            elif name == "AND":
                b = pop(); a = pop()
                push(F1 if (a != F0 and b != F0) else F0)
            elif name == "OR":
                b = pop(); a = pop()
                push(F1 if (a != F0 or b != F0) else F0)
            elif name == "NOT":
                push(F1 if pop() == F0 else F0)
            elif name == "EMA":
                n = pop(); x = pop()
                off = soff[arg]
                n_eff = max(int(n), 1)
                if state[off] == F0:
                    state[off + 1] = x
                else:
                    alpha = F(F(2.0) / F(n_eff + 1))
                    state[off + 1] = F(state[off + 1] + F(alpha * F(x - state[off + 1])))
                state[off] = F(state[off] + F1)
                push(state[off + 1])
            elif name == "RSI":
                n = pop(); x = pop()
                off = soff[arg]
                n_eff = max(int(n), 1)
                count = int(state[off])
                if count == 0:
                    res = F(50.0)
                else:
                    delta = F(x - state[off + 1])
                    g = F(max(delta, F0))
                    lo_ = F(max(F(-delta), F0))
                    if count == 1:
                        state[off + 2] = g
                        state[off + 3] = lo_
                    else:
                        state[off + 2] = F(state[off + 2] + F(F(g - state[off + 2]) / F(n_eff)))
                        state[off + 3] = F(state[off + 3] + F(F(lo_ - state[off + 3]) / F(n_eff)))
                    denom = F(state[off + 2] + state[off + 3])
                    res = F(50.0) if denom == F0 else F(F(100.0) * F(state[off + 2] / denom))
                state[off + 1] = x
                state[off] = F(state[off] + F1)
                push(res)
            elif name == "ATR":
                n = pop()
                off = soff[arg]
                n_eff = max(int(n), 1)
                if t == 0:
                    tr = F(h[t] - l[t])
                else:
                    pc = c[t - 1]
                    tr = F(max(F(h[t] - l[t]), max(F(abs(F(h[t] - pc))), F(abs(F(l[t] - pc))))))
                if state[off] == F0:
                    state[off + 1] = tr
                else:
                    state[off + 1] = F(state[off + 1] + F(F(tr - state[off + 1]) / F(n_eff)))
                state[off] = F(state[off] + F1)
                push(state[off + 1])
            elif name in ("SMA", "HIGHEST", "LOWEST", "STDDEV"):
                n = pop(); x = pop()
                off, cap = soff[arg], scap[arg]
                n_eff = min(max(int(n), 1), cap)
                count = int(state[off])
                state[off + 1 + count % cap] = x
                count += 1
                state[off] = F(count)
                k = min(count, n_eff)
                base = count - k
                if name == "SMA":
                    acc = F0
                    for i in range(k):
                        acc = F(acc + state[off + 1 + (base + i) % cap])
                    push(F(acc / F(k)))
                elif name == "HIGHEST":
                    acc = state[off + 1 + base % cap]
                    for i in range(1, k):
                        acc = F(max(acc, state[off + 1 + (base + i) % cap]))
                    push(acc)
                elif name == "LOWEST":
                    acc = state[off + 1 + base % cap]
                    for i in range(1, k):
                        acc = F(min(acc, state[off + 1 + (base + i) % cap]))
                    push(acc)
                else:
                    acc = F0
                    for i in range(k):
                        acc = F(acc + state[off + 1 + (base + i) % cap])
                    mean = F(acc / F(k))
                    ss = F0
                    for i in range(k):
                        d = F(state[off + 1 + (base + i) % cap] - mean)
                        ss = F(ss + F(d * d))
                    var = F(ss / F(k))
                    push(F(np.sqrt(var)) if var > F0 else F0)
            elif name in ("SMA_RAW", "HIGHEST_RAW", "LOWEST_RAW", "STDDEV_RAW"):
                n = pop()
                cap = scap[arg]
                sid, lag = saux[arg] >> 16, saux[arg] & 0xFFFF
                s = (o, h, l, c, v)[sid]
                n_eff = min(max(int(n), 1), cap)
                k = min(t + 1, n_eff)
                xs = [s[max(j - lag, 0)] for j in range(t - k + 1, t + 1)]
                if name == "SMA_RAW":
                    acc = F0
                    for xi in xs:
                        acc = F(acc + xi)
                    push(F(acc / F(k)))
                elif name == "HIGHEST_RAW":
                    acc = xs[0]
                    for xi in xs[1:]:
                        acc = F(max(acc, xi))
                    push(acc)
                elif name == "LOWEST_RAW":
                    acc = xs[0]
                    for xi in xs[1:]:
                        acc = F(min(acc, xi))
                    push(acc)
                else:
                    acc = F0
                    for xi in xs:
                        acc = F(acc + xi)
                    mean = F(acc / F(k))
                    ss = F0
                    for xi in xs:
                        d = F(xi - mean)
                        ss = F(ss + F(d * d))
                    var = F(ss / F(k))
                    push(F(np.sqrt(var)) if var > F0 else F0)
            elif name == "DELAY_RAW":
                kk = pop()
                cap = scap[arg]
                sid, lag = saux[arg] >> 16, saux[arg] & 0xFFFF
                s = (o, h, l, c, v)[sid]
                k_eff = min(max(int(kk), 0), cap - 1)
                avail = min(t + 1, cap)
                back = min(k_eff, avail - 1)
                push(s[max(t - back - lag, 0)])
            elif name == "DELAY":
                kk = pop(); x = pop()
                off, cap = soff[arg], scap[arg]
                count = int(state[off])
                state[off + 1 + count % cap] = x
                count += 1
                state[off] = F(count)
                k_eff = min(max(int(kk), 0), cap - 1)
                avail = min(count, cap)
                back = min(k_eff, avail - 1)
                push(state[off + 1 + (count - 1 - back) % cap])
            elif name in ("CROSSOVER", "CROSSUNDER"):
                b = pop(); a = pop()
                off = soff[arg]
                count = int(state[off])
                if name == "CROSSOVER":
                    r = count >= 1 and a > b and state[off + 1] <= state[off + 2]
                else:
                    r = count >= 1 and a < b and state[off + 1] >= state[off + 2]
                state[off + 1] = a
                state[off + 2] = b
                state[off] = F(count + 1)
                push(F1 if r else F0)
            elif name == "SIG_EL":
                sig_el = sig_el or pop() != F0
            elif name == "SIG_XL":
                sig_xl = sig_xl or pop() != F0
            elif name == "SIG_ES":
                sig_es = sig_es or pop() != F0
            elif name == "SIG_XS":
                sig_xs = sig_xs or pop() != F0
            elif name == "SET_STOP":
                cfg_stop = pop()
            elif name == "SET_TP":
                cfg_tp = pop()
            elif name == "SET_TRAIL":
                cfg_trail = pop()
            elif name == "SET_SIZE":
                cfg_size = F(min(max(pop(), F0), F1))
            elif name == "HALT":
                break
            else:
                raise RuntimeError(f"bad opcode {name}")

        rows.append([t, ctx_position, float(ctx_entry_px), float(eq_now),
                     int(sig_el), int(sig_xl), int(sig_es), int(sig_xs),
                     float(cfg_stop), float(cfg_tp), float(cfg_trail), float(cfg_size)]
                    + [float(x) for x in locals_[:program.n_locals]])

        if side > 0 and sig_xl:
            pend_action = 2
        elif side < 0 and sig_xs:
            pend_action = 2
        elif side == 0:
            if sig_el and not sig_es:
                pend_action, pend_size = 1, cfg_size
            elif sig_es and not sig_el:
                pend_action, pend_size = -1, cfg_size

        equity_curve[t] = F(cash + qsigned() * c[t])
        if side > 0:
            anchor = F(max(anchor, h[t]))
        elif side < 0:
            anchor = F(min(anchor, l[t]))

    if side != 0:
        px = F(c[n_bars - 1] * F(F1 - slip)) if side > 0 else F(c[n_bars - 1] * F(F1 + slip))
        close_pos(n_bars - 1, px, "eod")
        equity_curve[n_bars - 1] = cash

    return rows, equity_curve, trades


def cross_check(program: Program, bars, cfg, equity_curve, trades, rows) -> None:
    """The instrumented copy must reproduce refengine.run exactly."""
    ref = refengine.run(program, bars, config=cfg, record_locals=True)
    assert np.array_equal(ref.equity, equity_curve), "equity curve diverged"
    assert len(ref.trades) == len(trades), "trade count diverged"
    for a, b in zip(ref.trades, trades):
        assert (a.side, a.entry_t, a.exit_t, a.entry_px, a.exit_px, a.qty,
                a.pnl, a.reason) == \
               (b.side, b.entry_t, b.exit_t, b.entry_px, b.exit_px, b.qty,
                b.pnl, b.reason), "trade diverged"
    if ref.locals_curve is not None:
        got = np.asarray([r[12:] for r in rows], dtype=np.float32)
        assert np.array_equal(ref.locals_curve, got), "locals diverged"


def main() -> None:
    strat_dir = os.path.join(PROJ, "data", "strategies")
    out_dir = os.path.join(strat_dir, "golden")
    os.makedirs(out_dir, exist_ok=True)

    bars = make_bars()
    with open(os.path.join(out_dir, "bars.csv"), "w", encoding="utf-8") as f:
        f.write("t,o,h,l,c,v\n")
        for t in range(T):
            f.write(f"{t},{bars.open[t]:.9g},{bars.high[t]:.9g},{bars.low[t]:.9g},"
                    f"{bars.close[t]:.9g},{bars.volume[t]:.9g}\n")

    cfg = refengine.BacktestConfig()
    for name in STRATEGIES:
        dsl_path = os.path.join(strat_dir, f"{name}.dsl")
        with open(dsl_path, encoding="utf-8") as f:
            program = compile_source(f.read())
        export_file(dsl_path, os.path.join(strat_dir, f"{name}.obp"))  # keep .obp in sync

        rows, equity_curve, trades = instrumented_run(program, bars, cfg)
        cross_check(program, bars, cfg, equity_curve, trades, rows)

        out = os.path.join(out_dir, f"{name}_golden.csv")
        with open(out, "w", encoding="utf-8") as f:
            loc_hdr = "".join(f",local{i}" for i in range(program.n_locals))
            f.write("t,position,entry_px,equity,el,xl,es,xs,stop,tp,trail,size"
                    + loc_hdr + "\n")
            for row in rows:
                f.write(f"{row[0]},{row[1]},{row[2]:.9g},{row[3]:.9g},"
                        f"{row[4]},{row[5]},{row[6]},{row[7]},"
                        + ",".join(f"{x:.9g}" for x in row[8:]) + "\n")
        n_sig = sum(r[4] + r[5] + r[6] + r[7] for r in rows)
        print(f"{name}: {len(rows)} bars, {n_sig} signal firings, "
              f"{len(trades)} ref trades -> {out}")


if __name__ == "__main__":
    main()
