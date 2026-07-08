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

## 2. Real NASDAQ ITCH sample files (optional)

NASDAQ publishes full-day historical ITCH 5.0 recordings:

- <ftp://emi.nasdaq.com/ITCH/> (e.g. `01302019.NASDAQ_ITCH50.gz`)
- Protocol spec: search "NASDAQ TotalView-ITCH 5.0 specification".

Download, `gunzip`, and point the binary at it:

```bash
build/orderbook_sw path/to/01302019.NASDAQ_ITCH50 --symbol 1 --csv build/csv
```

> Note: real ITCH 5.0 uses a **6-byte** timestamp; this project uses an
> **8-byte** timestamp internally (SW, RTL, and the generator all agree, so
> everything here is self-consistent).
> To ingest unmodified NASDAQ files, set the timestamp width to 6 bytes in
> `sw/parser/itch_messages.hpp` (`hdr::TIMESTAMP`/body lengths) and the matching
> `rtl/ob_pkg.sv` constants. The synthetic generator and the whole test suite
> use the 8-byte convention end to end.

Real ITCH files are **not committed** to the repository (size + licensing).
