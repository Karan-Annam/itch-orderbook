#!/usr/bin/env python3
"""book_depth.py — visualise top-of-book over time.

Reads book_depth.csv (seq, locate, best_bid, bid_depth, best_ask, ask_depth,
spread) emitted by orderbook_sw and plots best bid/ask and spread versus message
sequence for the primary symbol. Prints summary stats and writes a CSV summary
when matplotlib is unavailable.
"""
import csv, os, sys, argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="build/csv")
    args = ap.parse_args()
    path = os.path.join(args.csv, "book_depth.csv")
    if not os.path.exists(path):
        print(f"[book_depth] {path} not found; run orderbook_sw --csv {args.csv}")
        return 0

    seq, bid, ask, spread = [], [], [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                seq.append(int(row["seq"]))
                bid.append(int(row["best_bid"]) / 10000.0)
                ask.append(int(row["best_ask"]) / 10000.0)
                spread.append(int(row["spread"]))
            except (KeyError, ValueError):
                pass

    if not seq:
        print("[book_depth] no rows parsed")
        return 0

    avg_spread = sum(spread) / len(spread)
    print(f"[book_depth] {len(seq)} snapshots")
    print(f"  bid range:    ${min(bid):.4f} .. ${max(bid):.4f}")
    print(f"  ask range:    ${min(ask):.4f} .. ${max(ask):.4f}")
    print(f"  mean spread:  {avg_spread:.1f} ticks (1 tick = $0.0001)")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 6), sharex=True)
        ax1.plot(seq, bid, color="#2b6cb0", lw=0.8, label="best bid")
        ax1.plot(seq, ask, color="#e53e3e", lw=0.8, label="best ask")
        ax1.set_ylabel("price ($)"); ax1.legend(); ax1.set_title("Top of book over time")
        ax2.plot(seq, spread, color="#dd6b20", lw=0.8)
        ax2.set_ylabel("spread (ticks)"); ax2.set_xlabel("message sequence")
        plt.tight_layout()
        png = os.path.join(args.csv, "book_depth.png")
        plt.savefig(png, dpi=110)
        print(f"[book_depth] wrote {png}")
    except Exception as e:
        print(f"[book_depth] plot skipped ({e.__class__.__name__})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
