#!/usr/bin/env python3
"""Create a machine-readable summary from freshly generated artifacts."""
import argparse
import csv
import json
import os
import platform
import re
from datetime import datetime, timezone


def counters(path):
    if not os.path.exists(path):
        return {}
    with open(path, newline="") as handle:
        return {row["counter"]: float(row["value"])
                for row in csv.DictReader(handle)}


def benchmark(path):
    if not os.path.exists(path):
        return {}
    text = open(path, encoding="utf-8").read()
    result = {}
    tier = re.search(r"SIMD tier: ([^\r\n]+)", text)
    speedup = re.search(r"Speedup:\s*([0-9.]+)x", text)
    rows = re.findall(r"^(std::map \(RB-tree\)|direct-index \+ SIMD)\s+"
                      r"([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$", text, re.M)
    if tier:
        result["simd_tier"] = tier.group(1).strip()
    if speedup:
        result["speedup_x"] = float(speedup.group(1))
    for name, elapsed_ms, mmsg_s, ns_msg in rows:
        key = "std_map" if name.startswith("std::map") else "direct_index_simd"
        result[key] = {"elapsed_ms": float(elapsed_ms),
                       "million_messages_per_second": float(mmsg_s),
                       "ns_per_message": float(ns_msg)}
    return result


def fpga_reports(build_dir):
    timing_path = os.path.join(build_dir, "timing_route_urbana.rpt")
    util_path = os.path.join(build_dir, "util_route_urbana.rpt")
    drc_path = os.path.join(build_dir, "drc_route_urbana.rpt")
    route_path = os.path.join(build_dir, "route_status_urbana.rpt")
    bitstream_path = os.path.join(build_dir, "fpga_top_urbana.bit")
    if not all(os.path.exists(path)
               for path in (timing_path, util_path, drc_path, route_path)):
        return {"status": "not_generated"}

    timing = open(timing_path, encoding="utf-8", errors="replace").read()
    util = open(util_path, encoding="utf-8", errors="replace").read()
    drc = open(drc_path, encoding="utf-8", errors="replace").read()
    route = open(route_path, encoding="utf-8", errors="replace").read()
    result = {
        "status": "generated",
        "target_mhz": 100.0,
        "bitstream_generated": os.path.exists(bitstream_path),
    }
    if result["bitstream_generated"]:
        result["bitstream_bytes"] = os.path.getsize(bitstream_path)
    match = re.search(r"WNS\(ns\).*?\n[-+\s]*\n\s*([-+]?\d+\.\d+)", timing, re.S)
    if match:
        result["wns_ns"] = float(match.group(1))
        result["timing_met"] = result["wns_ns"] >= 0

    resources = {}
    labels = {"Slice LUTs": "lut", "Slice Registers": "ff",
              "Block RAM Tile": "bram_tiles", "DSPs": "dsp"}
    for line in util.splitlines():
        fields = [field.strip() for field in line.split("|")[1:-1]]
        if len(fields) != 6 or fields[0] not in labels:
            continue
        label, used, _fixed, _prohibited, available, percent = fields
        resources[labels[label]] = {
            "used": float(used), "available": float(available),
            "percent": float(percent),
        }
    result["resources"] = resources

    routing_errors = re.search(
        r"# of nets with routing errors\.*\s*:\s*(\d+)", route)
    result["routing_error_nets"] = (
        int(routing_errors.group(1)) if routing_errors else None)

    violations = re.search(r"Violations found:\s*(\d+)", drc)
    result["drc_warning_count"] = int(violations.group(1)) if violations else None
    result["bram_async_reset_warnings"] = len(
        re.findall(r"^REQP-18(?:39|40)#", drc, re.M))
    return result


def integral_values(values):
    return {key: int(value) if value.is_integer() else value
            for key, value in values.items()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", default="build/csv")
    parser.add_argument("--output", default="docs/results.json")
    args = parser.parse_args()

    software = integral_values(counters(os.path.join(args.csv, "perf_counters_simd.csv")))
    hardware = integral_values(counters(os.path.join(args.csv, "perf_counters_hw.csv")))
    if not software or not hardware:
        raise SystemExit("missing software or RTL performance counters")

    document = {
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "host": {"system": platform.system(), "machine": platform.machine(),
                 "processor": platform.processor()},
        "workload": {"software_messages": software.get("messages"),
                     "rtl_messages": hardware.get("messages")},
        "software": software,
        "book_benchmark": benchmark(os.path.join(args.csv, "benchmark.txt")),
        "rtl_cycle_accurate": hardware,
        "fpga_route": fpga_reports("build"),
    }
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"[write_results] wrote {args.output}")


if __name__ == "__main__":
    main()
