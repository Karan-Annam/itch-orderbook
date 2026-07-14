#!/usr/bin/env python3
"""Compare software latency with cycle-accurate RTL end-to-end latency.

Software histograms are measured in nanoseconds. RTL histograms are stored in
cycles and converted only with the implemented clock recorded by tb_latency.
"""
import argparse
import csv
import os
import sys


PERCENTILES = (0.50, 0.95, 0.99, 0.999, 0.9999)
LABEL = {0.50: "p50", 0.95: "p95", 0.99: "p99", 0.999: "p99.9", 0.9999: "p99.99"}


def load_hist(path, value_column):
    points = []
    if not os.path.exists(path):
        return points
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                points.append((int(row[value_column]), int(row["count"])))
            except (KeyError, ValueError):
                pass
    return points


def load_counters(path):
    if not os.path.exists(path):
        return {}
    with open(path, newline="") as f:
        return {row["counter"]: float(row["value"]) for row in csv.DictReader(f)}


def percentiles(hist):
    total = sum(count for _, count in hist)
    if not total:
        return {}
    result = {}
    cumulative = 0
    for value, count in sorted(hist):
        cumulative += count
        for p in PERCENTILES:
            if p not in result and cumulative >= p * total:
                result[p] = value
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="build/csv")
    args = ap.parse_args()
    directory = args.csv

    sw_hist = load_hist(os.path.join(directory, "latency_simd_all.csv"), "latency_ns")
    rtl_hist = load_hist(os.path.join(directory, "latency_hw_e2e_all.csv"),
                         "latency_cycles")
    counters = load_counters(os.path.join(directory, "perf_counters_hw.csv"))
    clock_mhz = counters.get("clock_mhz", 0)
    if not sw_hist or not rtl_hist or clock_mhz <= 0:
        print("[latency_compare] missing software, RTL, or clock data", file=sys.stderr)
        return 1

    sw = percentiles(sw_hist)
    rtl_cycles = percentiles(rtl_hist)
    rtl_ns = {p: rtl_cycles[p] * 1000.0 / clock_mhz for p in PERCENTILES}

    print(f"=== Software RDTSC vs RTL end-to-end at {clock_mhz:.0f} MHz ===")
    print(f"{'percentile':>12} {'software ns':>14} {'RTL cycles':>12} {'RTL ns':>10}")
    summary = os.path.join(directory, "latency_summary.csv")
    with open(summary, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["percentile", "software_ns", "rtl_cycles", "rtl_ns", "clock_mhz"])
        for p in PERCENTILES:
            print(f"{LABEL[p]:>12} {sw[p]:>14.0f} {rtl_cycles[p]:>12.0f} {rtl_ns[p]:>10.0f}")
            writer.writerow([LABEL[p], f"{sw[p]:.0f}", f"{rtl_cycles[p]:.0f}",
                             f"{rtl_ns[p]:.0f}", f"{clock_mhz:.0f}"])
    print(f"[latency_compare] wrote {summary}")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        xs = [value for value, _ in sorted(sw_hist)]
        ys = [count for _, count in sorted(sw_hist)]
        plt.figure(figsize=(8, 5))
        plt.bar(xs, ys, width=1.0, color="#2b6cb0", label="software (RDTSC)")
        plt.axvline(rtl_ns[0.50], color="#e53e3e", ls="--",
                    label=f"RTL p50 {rtl_cycles[0.50]} cyc / {rtl_ns[0.50]:.0f} ns")
        plt.axvline(rtl_ns[0.999], color="#dd6b20", ls=":",
                    label=f"RTL p99.9 {rtl_cycles[0.999]} cyc / {rtl_ns[0.999]:.0f} ns")
        plt.xlim(0, max(1000, sw[0.999] * 1.2, rtl_ns[0.999] * 1.2))
        plt.xlabel("latency (ns)")
        plt.ylabel("count")
        plt.title(f"Order-book latency: software vs RTL at {clock_mhz:.0f} MHz")
        plt.legend()
        plt.tight_layout()
        output = os.path.join(directory, "latency_compare.png")
        plt.savefig(output, dpi=110)
        print(f"[latency_compare] wrote {output}")
    except Exception as exc:
        print(f"[latency_compare] plot skipped ({exc.__class__.__name__}); CSV written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
