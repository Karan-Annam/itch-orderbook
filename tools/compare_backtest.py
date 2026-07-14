"""Analytic backtest vs queue-aware execution, side by side.

Feeds the bars a trading run actually traded (bars.csv from orderbook_trade
--csv) into backtest_project's reference engine with the same strategy, then
prints both engines' results next to each other. The difference is pure
execution modeling: refengine fills at next-bar-open with fixed slippage;
the trading engine queued real orders in the replayed book.

Usage (from orderbook_project/):
    python tools/compare_backtest.py out/bars.csv data/strategies/sma_cross.dsl out/ \
        [--backtest ../backtest_project] [--fee 0.001] [--slip 0.0005] [--equity0 1e9]

The third argument is the CSV directory orderbook_trade wrote (needs
trades.csv and equity.csv alongside bars.csv).
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

parser = argparse.ArgumentParser()
parser.add_argument("bars_csv")
parser.add_argument("dsl")
parser.add_argument("run_dir", help="directory with trades.csv/equity.csv")
parser.add_argument("--backtest", default=os.path.join(PROJ, "..", "backtest_project"))
parser.add_argument("--fee", type=float, default=0.001)
parser.add_argument("--slip", type=float, default=0.0005)
parser.add_argument("--equity0", type=float, default=1e9)
args = parser.parse_args()
sys.path.insert(0, os.path.abspath(args.backtest))

import numpy as np  # noqa: E402

from dsl import refengine  # noqa: E402
from dsl.compiler import compile_source  # noqa: E402


class Bars:
    def __init__(self, o, h, l, c, v):
        self.open, self.high, self.low, self.close, self.volume = o, h, l, c, v


def load_bars(path: str) -> Bars:
    cols = {k: [] for k in "ohlcv"}
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            for k in "ohlcv":
                cols[k].append(float(row[k]))
    return Bars(*(np.asarray(cols[k], dtype=np.float32) for k in "ohlcv"))


def load_run(run_dir: str) -> dict:
    trades = []
    with open(os.path.join(run_dir, "trades.csv"), newline="", encoding="utf-8") as f:
        trades = list(csv.DictReader(f))
    equity = []
    with open(os.path.join(run_dir, "equity.csv"), newline="", encoding="utf-8") as f:
        equity = [float(r["equity"]) for r in csv.DictReader(f)]
    eq = np.asarray(equity, dtype=np.float64)
    peak = np.maximum.accumulate(eq)
    max_dd = float(np.max((peak - eq) / np.where(peak > 0, peak, 1))) if len(eq) else 0.0
    wins = sum(1 for t in trades if float(t["pnl"]) > 0)
    final = eq[-1] if len(eq) else args.equity0
    return {
        "final_equity": float(final),
        "total_return": float(final / args.equity0 - 1.0),
        "max_drawdown": max_dd,
        "n_trades": len(trades),
        "win_rate": wins / len(trades) if trades else 0.0,
    }


def main() -> None:
    bars = load_bars(args.bars_csv)
    with open(args.dsl, encoding="utf-8") as f:
        program = compile_source(f.read())

    cfg = refengine.BacktestConfig(fee_rate=args.fee, slip=args.slip,
                                   equity0=args.equity0)
    ref = refengine.run(program, bars, config=cfg)
    a = ref.metrics
    b = load_run(args.run_dir)

    rows = [
        ("final equity", f"{a['final_equity']:,.0f}", f"{b['final_equity']:,.0f}"),
        ("total return", f"{a['total_return'] * 100:+.4f}%", f"{b['total_return'] * 100:+.4f}%"),
        ("max drawdown", f"{a['max_drawdown'] * 100:.4f}%", f"{b['max_drawdown'] * 100:.4f}%"),
        ("trades", str(a["n_trades"]), str(b["n_trades"])),
        ("win rate", f"{a['win_rate'] * 100:.1f}%", f"{b['win_rate'] * 100:.1f}%"),
    ]
    name = os.path.basename(args.dsl)
    print(f"\n{name}: same strategy, same bars - different execution model\n")
    hdr = f"{'':16}{'analytic (next-open fills)':>28}{'queue-aware (real book)':>28}"
    print(hdr)
    print("-" * len(hdr))
    for label, av, bv in rows:
        print(f"{label:16}{av:>28}{bv:>28}")

    gap = a["total_return"] - b["total_return"]
    print(f"\nexecution gap: {gap * 100:+.4f}% of return (analytic minus realized).")
    print("Positive: the analytic model overstated performance (real fills paid")
    print("spread/queue costs its fixed-slippage assumption hides). Negative: the")
    print("fixed-slippage assumption was more pessimistic than execution on this")
    print("stream (e.g. passive joins earned the spread). Compare --entry join")
    print("vs cross on the same seed to isolate the spread cost itself.\n")


if __name__ == "__main__":
    main()
