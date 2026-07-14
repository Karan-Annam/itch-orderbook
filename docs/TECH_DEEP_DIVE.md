# Technical Deep Dive — ITCH Order Book, C++ and RTL

Detailed implementation and verification notes that complement
[DESIGN.md](DESIGN.md) with structural detail. Current status and planned work
are tracked in [STATE_AND_ROADMAP.md](STATE_AND_ROADMAP.md).

---

## 1. What this project is, in one paragraph

The same NASDAQ ITCH 5.0 limit order book built twice — SIMD-tuned C++17 and
synthesizable SystemVerilog — consuming the *same byte stream* and required to
produce *bit-identical book state* (the RTL is diffed against a `std::map`
reference model after every message). The deliverable is a **measured**
software/hardware latency comparison instead of folklore: software p50 ≈
190 ns/message with a ~7× tail; hardware (after a front-end redesign) services
a message every 15 cycles = 60 ns at a modeled 250 MHz, with p99.9 = p99 —
the determinism, not the median, being hardware's real advantage.

## 2. The wire-format contract

- ITCH 5.0: length-prefixed, big-endian, packed back-to-back with **no
  alignment**. Messages used: `A` Add (38 B body), `F` Add-MPID (42),
  `E` Execute (33), `C` Execute-with-price (38), `X` Cancel (25), `D` Delete
  (21), `U` Replace (37), `P` Trade (46), `S` System (14). Common 13-byte
  header: type, stock_locate(2), tracking(2), timestamp(8).
- The layouts exist in exactly **two mirrored files** —
  `sw/parser/itch_messages.hpp` and `rtl/ob_pkg.sv` — and the end-to-end diff
  enforces their agreement by test, not discipline.
- Documented deviation: 8-byte timestamps internally (real ITCH uses 6 on the
  wire); generator, software, and RTL are self-consistent, and `data/README.md`
  describes the 6→8 adjustment for real NASDAQ files.
- Byte order: the C++ parser byte-swaps with intrinsics; the RTL "swaps" by
  wiring bytes in reverse order — zero gates, a nice example of work that is
  free in hardware and real work on a CPU.

## 3. Software pipeline (`sw/`)

```
receiver (file replay | raw socket | AF_XDP) → SPSC ring → parser
       → order-ref hash table → direct-indexed book → stats engine
```

### 3.1 Order-ref hash table (`sw/book/order_ref_table.hpp`)

The core data-structure problem: Execute/Cancel/Delete/Replace identify an
order **only by a 64-bit reference number**, so you need
`ref → (price, side, shares, locate)` at O(1) on the hot path.

- **Open addressing, linear probing**, capacity 2²⁰ slots default, power-of-two
  masked; load factor kept < 0.7; key 0 reserved as the empty sentinel.
- **Hash: Thomas Wang's mix64** (~5 ALU ops of shift/xor/add). Chosen over
  multiplicative hashes for measured low collision rates on the generator's
  mostly-sequential reference numbers — sequential keys are the adversarial
  case for weak hashes.
- **Structure-of-arrays**: `keys_[]` is a dense `uint64` array — 8 keys per
  64-byte cache line — with values in a parallel array. This is what makes the
  probe SIMD-able: one aligned 64-byte load = one `_mm512_cmpeq_epi64` over 8
  candidate slots (AVX-512) or two `_mm256_cmpeq_epi64` over 4 (AVX2). AoS
  would need gathers.
- **Backward-shift deletion (Knuth 6.4-R)**, not tombstones: on erase, walk
  the chain forward and shift back any entry whose home slot lies cyclically
  outside `(deleted, current]` — the `in_gap` wrap-around check is the subtle
  line. Result: chains never accumulate dead slots, so lookup cost is stable
  over a full session. (Contrast with the RTL, which chose tombstones — §4.3.)
- Probe-depth stats recorded per op; a **200k-operation differential test
  against `std::unordered_map`** (same op sequence, full state compare) is the
  correctness anchor.

### 3.2 The book (`sw/book/order_book.hpp`)

- **Price levels are a direct-indexed array**: price *is* the index (2²⁰
  levels in RTL terms; the C++ side sizes similarly). An update is
  load→add→store — no tree, no allocation, no pointer chase.
- Best bid/ask maintained **incrementally**: an Add at a better price updates
  best in O(1); the only expensive case is deleting the last shares at the
  best, which triggers a **vectorized scan** (AVX compare of 8/4 levels per
  step) toward the interior until the next non-empty level.
- The original `std::map` book is retained as `reference_book.hpp` — it is
  simultaneously the correctness oracle and the benchmark baseline. Measured:
  direct+SIMD ≈ 2× faster (~90–110 vs ~200–235 ns/msg book-only at 1M
  messages; run-to-run 1.5–2.6×).
- Trade-off to own: direct indexing burns memory proportional to the price
  range and only works because equity prices are bounded and dense near the
  top of book; `std::map` handles any price domain. This is a
  domain-knowledge-for-speed trade, the essence of HFT data structures.

### 3.3 Receivers and measurement

- Receiver hierarchy: portable file-replay (default), raw socket + busy poll,
  AF_XDP zero-copy — the kernel-bypass ones are Linux-only and
  `#ifdef`-guarded; the downstream hot path is identical, so published numbers
  don't depend on receiver choice. Between receiver and parser sits a
  **lock-free SPSC ring** (single producer, single consumer, power-of-two
  masking, acquire/release semantics).
- **Measurement**: RDTSC pairs around parse+update per message, bucketed into
  per-message-type histograms (p50/p95/p99/p99.9/p99.99). Numbers on this
  machine (AVX2 tier): all-message p50 ≈ 190 ns, p99 ≈ 540 ns, p99.9 ≈ 1.3 µs;
  Trade `P` (no book op) ≈ 35 ns p50; Replace `U` (worst: delete + insert)
  ≈ 260 ns p50 / 650 ns p99.
- Every SIMD routine has AVX-512 / AVX2 / scalar paths selected at compile
  time — **the scalar path is the test oracle** for the vector paths.

## 4. Hardware pipeline (`rtl/`)

```
udp_stripper → itch_framer → itch_decoder → book_update_engine → stats_engine
                (32B window)   (reg'd dec)     ├ order_ref_table (64K SRAM)
   16 B/cycle ingest bus                       ├ best_tracker ×2 (bid/ask)
                                               └ bid/ask level SRAMs (2^20)
                                             perf_counters
```

Key parameters (`ob_pkg.sv`): 16-byte ingest bus (`WORD_BYTES=16`), 64K-slot
hash table (`TABLE_SIZE=2^16`, `MAX_PROBE=64`), 2²⁰ direct-indexed price
levels, 48-byte max message buffer.

### 4.1 The wide framer (`itch_framer.sv`) — compacting window

Problem: on a 16 B/cycle bus, a message can start/end at any byte offset, and
the 2-byte length prefix can straddle two words. Enumerating straddle cases as
FSM states explodes. Solution: a **32-byte compacting window** — each cycle,
consume from the front (2 bytes for a prefix, up to 16 for a body beat),
append the incoming word at the back. A split prefix stops being a special
case: it's just "window occupancy < 2, wait a cycle." Body bytes are
re-aligned on the way out so the decoder always sees message-aligned beats and
its field extraction stays pure wiring.

The invariant that keeps 32 bytes sufficient: the **minimum frame (2-byte
prefix + 14-byte System body) exactly equals the 16-byte word**, so at most
one message can end inside any ingest word — the window never tracks more
than "tail of current + head of next."

### 4.2 Ingest/update overlap — the free 1-deep buffer

Originally the framer parked until the engine finished each message
(serialized ingest + update ≈ 50 cycles/message). Observation: the decoder
already holds the decoded message in a register (`dec`) until the engine
accepts it — that register **is** a 1-deep skid buffer. Now the framer only
stalls when the *next* message is fully assembled and the engine still hasn't
taken the current one. Ingest of N+1 (3–4 cycles) hides entirely behind the
book update of N (8–20 cycles). Safety invariant: `msg_complete` is gated on
the decoder being empty, so `dec` is never overwritten while pending, and
`dec_valid` drops for ≥1 cycle between messages (clean single-message
handshake). A deeper FIFO was deliberately **not** added: the engine is the
bottleneck at 100% occupancy, so extra buffering buys nothing — measured
`ingest_stall_cycles` shows the engine is the limiter for 81% of the run.

### 4.3 RTL hash table (`order_ref_table.sv`) — and Bug #1

- Same open-addressing/linear-probing scheme as software, but the hash is an
  **XOR-fold** of the 64-bit ref into 16 bits (`r[63:48]^r[47:32]^r[31:16]^
  r[15:0]`) — pure wires, zero cycles, versus Wang mix64 which would be a
  multi-stage pipeline in hardware. One probe per cycle (async-read SRAM:
  read, compare, step).
- **Deletion uses tombstones** (`tomb` bit), the opposite choice from
  software: backward-shift would mean reading and rewriting SRAM entries one
  per cycle down the chain — miserable in hardware. Lookups probe *past*
  tombstones (only a true empty terminates a search); inserts remember the
  **first tombstone** seen (`have_tomb_q/tomb_idx_q`) and reuse it, so chains
  don't grow without bound. Tombstones aren't bulk-reclaimed — a very long
  session would eventually want a sweep/rehash (fine at 64K for these runs).
- **Bug #1 (the instructive one)**: the first version deleted by clearing the
  slot to empty. With linear probing, an emptied slot terminates every probe
  chain running through it — any colliding key stored beyond it becomes
  unreachable. Caught by a directed test that forces a 24-deep collision
  chain and deletes out of its middle, under the end-to-end reference diff.
- Probe-depth telemetry (1 / 2 / >2) pulses to `perf_counters` — visibility
  into hash quality is a first-class output, not an afterthought.

### 4.4 Best tracker (`best_tracker.sv`) — and Bug #2

Two instances (bid: best = highest, scans downward; ask: lowest, scans up).
Three behaviors:

- Add at a better price → best updates **in one cycle** (pure comparison).
- Best level emptied and side still has orders → **scan** one level per cycle
  from the adjacent price toward the interior until `shares != 0`.
- Side fully empty (`side_nonempty=0`, from the engine's per-side order
  counters) → clear `best_valid` in O(1), skipping the scan entirely — this
  guard is what prevents pathological million-level scans on an empty side.

**Bug #2 — the handshake race**: the engine kicked a scan and waited for
`!scanning` to declare the message done — but the tracker raises `scanning`
one cycle *after* sampling the kick. In that one-cycle window the engine could
commit with `best_price` still pointing at the just-emptied level (a stale,
zero-share best). Classic ready/busy race. Fix (in `book_update_engine.sv`):
compute whether a scan is *expected* for this message and wait to see it
actually start **and** finish (`expect_scan_q` / `scan_seen_q`).
`test_book_update` now pins the invariant "best price always has shares."

### 4.5 Book update engine (`book_update_engine.sv`)

One FSM serializes each message; owns the level SRAMs, the hash table, both
trackers, and per-side order counters. Message semantics worth knowing:

- **Replace `U`** = remove-then-add on the original side (two sequential
  sub-operations — the slowest message in both implementations, matching the
  software's ~260 ns).
- **Execute `E` carries no price** — the trade event is emitted at the
  order's *resting* price from the table lookup; `C` (execute-with-price)
  uses the message's price, gated on the printable flag; `P` passes through.
  These feed VWAP in `stats_engine` (also: spread, top-5 imbalance, depth).
- Costs: pure-Add stream commits every **8 cycles**; table-touching messages
  (E/X/D/U) take longer (probe + level update ± scan) → the 15-cycle p50 /
  21-cycle p99 service times on the mixed stream.

## 5. Verification

| layer | oracle | scale |
|---|---|---|
| SW hash table | `std::unordered_map` differential | 200k ops |
| SW book | `std::map` reference book, bit-for-bit | full streams |
| SW SIMD paths | scalar paths | per-routine |
| RTL modules | 8 directed Verilator testbenches (framer straddle cases, decoder fields, table incl. 24-deep chain + delete-from-middle, replace, overlap, UDP strip, full pipeline) | per-module |
| RTL end-to-end | C++ reference model, **full book state diffed after every message** | 20k+ message replays |

~140k assertions total on the software side. Both real bugs (§4.3, §4.4) were
found by the reference diff, not by happy-path tests — the project's central
verification lesson.

## 6. The results, and how to talk about them

| | software (AVX2) | HW byte-serial (v1) | HW wide+overlap (v2) |
|---|---|---|---|
| p50 service | ~190 ns | 200 ns (50 cyc) | **60 ns (15 cyc)** |
| p99 service | ~540 ns | 248 ns | 84 ns (21 cyc) |
| p99.9 | ~1.3 µs | = p99 | **= p99 (84 ns)** |
| throughput | — | ~5.0 M msg/s | **~20.9 M msg/s (4.2×)** |
| 20k messages | — | 1,006,092 cyc | 239,458 cyc |

The v1→v2 story is the strongest part: the byte-serial front end spent ~40 of
~50 cycles just shifting a 38-byte Add in — software's 190 ns median *beat*
the hardware. Measuring that (not assuming it) motivated the 16 B/cycle
framer + overlap, after which hardware wins the median too. The durable
argument is the tail: fixed-latency SRAM, no cache misses, no scheduler — the
p99.9 *equals* the p99, and the only tail contributor (best-price scan) is
bounded and attributed cycle-by-cycle in the perf counters. Software's tail is
~7× its median precisely during bursts — when everyone's software degrades at
once, which is when determinism is worth money.

Honest caveats, always stated: 250 MHz is a modeled clock, not post-P&R;
async-read SRAMs are LUTRAM-style and a real FPGA build re-maps them to
registered BRAM (+1 pipeline stage); single-symbol RTL vs multi-symbol
software; the 4–5 GHz OoO CPU still wins on raw clock — hardware wins by
doing *less per message* (no instruction stream), not by switching faster.
