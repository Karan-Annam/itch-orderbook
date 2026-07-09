#!/usr/bin/env python3
"""Software vs hardware latency distribution comparison.

Reads the software latency histogram CSVs emitted by orderbook_sw
(build/csv/latency_sw_all.csv and per-type files) and compares them against the
hardware (RTL) latency, which is deterministic and derived from the pipeline
cycle counts below (or from build/csv/latency_hw.csv if the RTL harness
produced one).

Plots both distributions on one axis when matplotlib is available; otherwise
prints a percentile table and writes latency_summary.csv. Never hard-fails.
"""
import csv, os, sys, argparse

# Hardware fallback: pipeline stage cycle counts, modeled at 250 MHz
# (4 ns/cycle). Used only when tb_latency's measured first-byte-to-commit
# histogram (latency_hw_e2e_all.csv) is absent. The front end ingests 16
# bytes/cycle, so a 40-byte Add frame is ~5 ingest cycles; decode is
# combinational wiring.
HW_NS_PER_CYCLE = 4.0
HW_STAGES = {  # cycles
    "ingest_16B": 5, "itch_decode": 1, "ref_lookup": 2,
    "book_update": 4, "best_tracker": 1,
}
HW_ADD_NS = sum(HW_STAGES.values()) * HW_NS_PER_CYCLE           # ~52 ns
HW_DELETE_NS = (sum(HW_STAGES.values()) + 9) * HW_NS_PER_CYCLE  # ~88 ns


def load_hist(path):
    """Return list of (latency_ns, count)."""
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


def percentiles(hist, ps=(0.50, 0.95, 0.99, 0.999, 0.9999)):
    total = sum(c for _, c in hist)
    if total == 0:
        return {p: 0 for p in ps}
    out, cum, i = {}, 0, 0
    sh = sorted(hist)
    targets = {p: p * total for p in ps}
    res = {}
    for lat, c in sh:
        cum += c
        for p in ps:
            if p not in res and cum >= targets[p]:
                res[p] = lat
    for p in ps:
        res.setdefault(p, sh[-1][0] if sh else 0)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="build/csv")
    args = ap.parse_args()
    d = args.csv

    sw = load_hist(os.path.join(d, "latency_sw_all.csv"))
    if not sw:
        print(f"[latency_compare] no SW latency CSV in {d}; run orderbook_sw --csv {d}")
        return 0

    sw_p = percentiles(sw)
    # Prefer the measured first-byte-to-commit histogram from tb_latency.
    hw_hist = load_hist(os.path.join(d, "latency_hw_e2e_all.csv"))
    if hw_hist:
        hw = percentiles(hw_hist)
        hw_src = "measured"
    else:
        hw = {0.50: HW_ADD_NS, 0.95: HW_ADD_NS, 0.99: HW_DELETE_NS,
              0.999: HW_DELETE_NS, 0.9999: HW_DELETE_NS}
        hw_src = "modeled"
    print(f"=== Software (RDTSC) vs Hardware (RTL, {hw_src}) latency ===")
    print(f"{'percentile':>12} {'software ns':>14} {'hardware ns':>14}")
    label = {0.50: "p50", 0.95: "p95", 0.99: "p99", 0.999: "p99.9", 0.9999: "p99.99"}
    with open(os.path.join(d, "latency_summary.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["percentile", "software_ns", "hardware_ns"])
        for p in (0.50, 0.95, 0.99, 0.999, 0.9999):
            print(f"{label[p]:>12} {sw_p[p]:>14.0f} {hw[p]:>14.0f}")
            w.writerow([label[p], f"{sw_p[p]:.0f}", f"{hw[p]:.0f}"])
    if hw_hist:
        print(f"\nHardware e2e p50 {hw[0.50]:.0f} ns, p99.9 {hw[0.999]:.0f} ns "
              f"(first byte to commit, at full input rate)")
    else:
        print(f"\nHardware Add latency  ~{HW_ADD_NS:.0f} ns (modeled, deterministic)")
    print(f"Software p50/p99 gap shows non-determinism (cache, OS jitter).")
    print(f"[latency_compare] wrote {d}/latency_summary.csv")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        xs = [lat for lat, _ in sorted(sw)]
        ys = [c for _, c in sorted(sw)]
        plt.figure(figsize=(8, 5))
        plt.bar(xs, ys, width=1.0, color="#2b6cb0", label="software (RDTSC)")
        if hw_hist:
            plt.axvline(hw[0.50], color="#e53e3e", ls="--",
                        label=f"HW e2e p50 {hw[0.50]:.0f}ns")
            plt.axvline(hw[0.999], color="#dd6b20", ls=":",
                        label=f"HW e2e p99.9 {hw[0.999]:.0f}ns")
        else:
            plt.axvline(HW_ADD_NS, color="#e53e3e", ls="--", label=f"HW Add ~{HW_ADD_NS:.0f}ns")
            plt.axvline(HW_DELETE_NS, color="#dd6b20", ls=":", label=f"HW Delete ~{HW_DELETE_NS:.0f}ns")
        plt.xlim(0, max(1000, sw_p[0.999] * 1.2))
        plt.xlabel("latency (ns)"); plt.ylabel("count")
        plt.title("Order book per-message latency: software vs hardware")
        plt.legend(); plt.tight_layout()
        out = os.path.join(d, "latency_compare.png")
        plt.savefig(out, dpi=110)
        print(f"[latency_compare] wrote {out}")
    except Exception as e:
        print(f"[latency_compare] plot skipped ({e.__class__.__name__}); CSV written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
