# Market data

This project processes **NASDAQ ITCH 5.0** order-event data. Two sources are
supported.

## 1. Synthetic data (default, used by the tests)

Real ITCH sample files are large and must be downloaded out of band, so the
project ships a deterministic **synthetic ITCH 5.0 generator** that emits a
byte-exact stream: correct 2-byte big-endian length prefixes, big-endian
fields, and the exact body layouts in `sw/parser/itch_messages.hpp`, covering
every message type the book handles (`A F E C X D U P S`).

The generator maintains its own set of live orders, so every Execute / Cancel /
Delete / Replace references an order that is actually resting and never
over-executes; resting bids are placed strictly below each symbol's mid and asks
strictly above it, so the resulting book is realistic and never crossed.

Generate a file:

```bash
make gen                                  # builds build/gen_itch
build/gen_itch data/sample.itch 200000 4  # <out> <num_messages> <num_symbols> [seed]
```

The same file is replayed by both the C++ software pipeline and the RTL
Verilator harness, which is what makes the software-vs-hardware comparison
apples-to-apples.

## 2. Real NASDAQ ITCH sample files

NASDAQ publishes free full-day TotalView-ITCH 5.0 recordings (several GB
gzipped). Raw days live in the gitignored `data/real/`:

```bash
curl -o data/real/12302019.NASDAQ_ITCH50.gz \
  "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
```

Real ITCH 5.0 uses a **6-byte** timestamp and an 11-byte header; the project
format keeps **8-byte** timestamps (deliberate: the RTL framer's 16-byte
minimum-frame invariant — at most one message end per ingest word — depends
on it; see `rtl/ob_pkg.sv`). `gui/data/nasdaq_itch.py` is the normalization
stage every real feed handler has anyway: it reads the .gz directly, keeps
the 9 modeled types, filters one symbol, and re-grids prices:

```bash
python -m gui.data.nasdaq_itch data/real/12302019.NASDAQ_ITCH50.gz \
    data/real/aapl_50k_scaled.itch \
    --symbol AAPL --max 50000 --tick-scale 100 --after-hm 09:35
python tools/band_filter.py data/real/aapl_50k_scaled.itch \
    data/real_sample_scaled.itch --window 1024
```

`band_filter.py` fits the stream into one 1024-tick window (median-of-adds
center): out-of-window Adds are dropped whole and Replaces that move an order
out of the window become Deletes — semantics both the software and the banded
hardware book apply identically, which is what makes the final-state diff
exact. It records `band_base` in the sidecar; the replay tools send it to the
hardware via the SET_BAND link frame before streaming. Start at 09:35, not
09:30 — the opening auction leaves a legitimately crossed book whose orders
are removed by Cross-Trade messages the project doesn't model.

`data/real_sample_scaled.itch` is the one committed real-data artifact: a
small regular-session excerpt used by `tools/run_real_data.sh` (software ==
full-RTL == banded-RTL triple agreement) and the hardware acceptance demo
(`tools/ob_host.py verify`).

### Why --tick-scale 100 and the band filter

The FPGA build's banded book covers a **1024-tick window**:

| tick grid | window span | verdict |
|-----------|-------------|---------|
| raw (1/10000 $) | $0.1024 | useless — any symbol walks out in minutes |
| cents (`--tick-scale 100`) | $10.24 | fits any symbol whose day range < $10 |

At cent ticks a $290 symbol sits at 29,000 — inside both the full simulation
window (2²⁰, so the reference-model diff runs on real data unchanged) and the
banded window. `tools/run_real_data.sh` asserts `band_drops == 0` and
`tbl_ins_fails == 0`; a volatile symbol that trips the former needs a larger
scale (500 = nickel grid). The `<file>.meta.json` sidecar records the scale,
locate codes, and band base.

Production sizing note: a real deployment would widen `PRICE_LEVELS` (BRAM
remap + band re-centering) rather than coarsen the grid; the tick scale is
the zero-RTL-churn way to run real data through the 1024-level demo build.
