#!/usr/bin/env python3
"""band_filter.py — fit a project-format .itch stream into one price-band
window so the banded FPGA book can replay it with zero drops.

  python tools/band_filter.py in.itch out.itch [--window 1024]

Picks base = median(add prices) - window/2, then:
  * Add/AddMPID outside [base, base+window)  -> dropped whole (their later
    Exec/Cancel/Delete/Replace become unknown-ref no-ops on BOTH the software
    and the banded hardware book — identical semantics, so final states agree)
  * Replace whose NEW price is outside       -> rewritten to a Delete of the
    original ref (exactly what "the order left the window" means; the banded
    RTL would apply the remove and drop the re-add, software would keep the
    moved order — the rewrite keeps both sides identical)
  * everything else passes through (Trade 'P' prices are VWAP-only, no level
    storage, so out-of-window prints are fine)

Writes/updates the <out>.meta.json sidecar with band_base + band_window; the
replay tools (tb_replay, ob_host.py) read it to pin the hardware band via the
config port / SET_BAND frame before streaming.
"""
import argparse
import json
import os
import statistics
import struct
import sys

HDR = 13  # type[1] + locate[2] + tracking[2] + timestamp[8]


def messages(data: bytes):
    o = 0
    while o + 2 <= len(data):
        blen = struct.unpack_from(">H", data, o)[0]
        if blen == 0 or o + 2 + blen > len(data):
            break
        yield data[o + 2:o + 2 + blen]
        o += 2 + blen


def frame(body: bytes) -> bytes:
    return struct.pack(">H", len(body)) + body


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src")
    ap.add_argument("dest")
    ap.add_argument("--window", type=int, default=1024,
                    help="band window in ticks (PRICE_LEVELS of the FPGA build)")
    args = ap.parse_args()

    with open(args.src, "rb") as f:
        data = f.read()

    add_prices = [struct.unpack_from(">I", m, 34)[0]
                  for m in messages(data) if m[0:1] in (b"A", b"F")]
    if not add_prices:
        print("no Add messages found", file=sys.stderr)
        return 1
    center = int(statistics.median(add_prices))
    base = max(center - args.window // 2, 0)
    lo, hi = base, base + args.window

    out = bytearray()
    stats = {"kept": 0, "adds_dropped": 0, "replaces_rewritten": 0}
    for m in messages(data):
        t = m[0:1]
        if t in (b"A", b"F"):
            px = struct.unpack_from(">I", m, 34)[0]
            if not (lo <= px < hi):
                stats["adds_dropped"] += 1
                continue
        elif t == b"U":
            px = struct.unpack_from(">I", m, 33)[0]
            if not (lo <= px < hi):
                # Delete of the original ref, same header/timestamp
                body = b"D" + m[1:HDR] + m[HDR:HDR + 8]
                out += frame(body)
                stats["replaces_rewritten"] += 1
                stats["kept"] += 1
                continue
        out += frame(m)
        stats["kept"] += 1

    with open(args.dest, "wb") as f:
        f.write(out)

    meta_path = args.dest + ".meta.json"
    meta = {}
    src_meta = args.src + ".meta.json"
    if os.path.exists(src_meta):
        with open(src_meta) as f:
            meta = json.load(f)
    meta.update({"band_base": base, "band_window": args.window,
                 "band_center": center, "output_msgs": stats["kept"]})
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)

    print(f"[band_filter] center={center} window=[{lo},{hi}) "
          f"kept={stats['kept']} adds_dropped={stats['adds_dropped']} "
          f"U->D={stats['replaces_rewritten']} -> {args.dest} ({len(out):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
