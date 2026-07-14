# Current State and Roadmap

## Verified now

| Component | Evidence |
|---|---|
| ITCH parser and software book | parser boundary tests and reference-book comparisons |
| Order-reference hash table | 200K-operation differential test against `std::unordered_map` |
| SIMD level scanning | scalar/AVX path equivalence checks |
| RTL frontend and book | directed Verilator tests and 20K-message state comparison |
| MoldUDP64 handling | software and RTL heartbeat, stale, and gap vectors |
| Board wrapper and UART-like link | bit-level board-chain simulation with corruption recovery |
| Spartan-7 implementation | routed timing, utilization, DRC, and bitstream artifacts |
| Measurement pipeline | serialized software timing and cycle-counted RTL histograms |

The default tests use generated data. Licensed NASDAQ captures are not stored in
the repository; `gui/data/nasdaq_itch.py` converts a user-supplied native file
from six-byte timestamps into the project's normalized eight-byte format.

The RTL currently maintains one symbol. Its FPGA build uses a 1,024-level price
window and a reduced order-reference table to fit the xc7s50 target. Raw-socket
and AF_XDP receivers are present for Linux, while the published measurements
use deterministic file replay. The routed board design has not yet been tested
on the physical board.

Generated evidence is recorded in [results.json](results.json). The RTL latency
numbers come from Verilator cycles; the Vivado report separately establishes
that the 100 MHz constraint is met.

## Next milestones

1. Program the Urbana board and exercise the host link with hardware-in-loop
   snapshots, corruption injection, and sustained replay.
2. Add a retransmit/session path for detected MoldUDP64 sequence gaps.
3. Add banked multi-symbol book state or a small book-state cache.
4. Recenter the FPGA price window when the best price approaches an edge.
5. Attribute tail latency by pipeline stage and compare against software
   profiles from the same message sequence.
6. Exercise and measure the Linux raw-socket and AF_XDP receive paths on a
   feed-capable host.
