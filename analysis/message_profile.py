#!/usr/bin/env python3
"""message_profile.py — per-message-type count and latency profile.

Reads per-type software latency histograms (latency_sw_<Type>.csv) and the
perf counters CSV, prints a table of count + p50/p99 per message type, and (if
matplotlib is present) draws a grouped bar chart. Delete/Replace are expected to
be the most expensive types (best-price rescan / sequential sub-ops).
"""
import csv, os, sys, argparse

TYPES = ["Add", "AddMPID", "Execute", "ExecPrice", "Cancel",
         "Delete", "Replace", "Trade", "System"]


def load_hist(path):
    pts = []
    if not os.path.exists(path):
        return pts
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                pts.append((int(row["latency_ns"]), int(row["count"])))
            except (KeyError, ValueError):
                pass
    return pts


def pct(hist, p):
    total = sum(c for _, c in hist)
    if not total:
        return 0
    cum = 0
    for lat, c in sorted(hist):
        cum += c
        if cum >= p * total:
            return lat
    return sorted(hist)[-1][0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="build/csv")
    args = ap.parse_args()
    d = args.csv

    rows = []
    for t in TYPES:
        h = load_hist(os.path.join(d, f"latency_sw_{t}.csv"))
        n = sum(c for _, c in h)
        if n:
            rows.append((t, n, pct(h, 0.50), pct(h, 0.99)))

    if not rows:
        print(f"[message_profile] no per-type CSVs in {d}; run orderbook_sw --csv {d}")
        return 0

    print(f"{'type':>10} {'count':>10} {'p50 ns':>8} {'p99 ns':>8}")
    out = os.path.join(d, "message_profile.csv")
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["type", "count", "p50_ns", "p99_ns"])
        for t, n, p50, p99 in rows:
            print(f"{t:>10} {n:>10} {p50:>8} {p99:>8}")
            w.writerow([t, n, p50, p99])
    print(f"[message_profile] wrote {out}")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        ts = [r[0] for r in rows]
        p50 = [r[2] for r in rows]
        p99 = [r[3] for r in rows]
        x = range(len(ts))
        plt.figure(figsize=(9, 5))
        plt.bar([i - 0.2 for i in x], p50, width=0.4, label="p50", color="#2b6cb0")
        plt.bar([i + 0.2 for i in x], p99, width=0.4, label="p99", color="#dd6b20")
        plt.xticks(list(x), ts, rotation=30)
        plt.ylabel("latency (ns)")
        plt.title("Per-message-type latency (software)")
        plt.legend(); plt.tight_layout()
        png = os.path.join(d, "message_profile.png")
        plt.savefig(png, dpi=110)
        print(f"[message_profile] wrote {png}")
    except Exception as e:
        print(f"[message_profile] plot skipped ({e.__class__.__name__}); CSV written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
