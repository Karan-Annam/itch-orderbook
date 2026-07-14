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
software/hardware latency comparison instead of folklore. The latest generated
result is 203 ns software p50 for parse plus update; RTL is 55 cycles p50
end-to-end and 19 cycles median service time at the implemented 100 MHz clock.
Cycles are reported directly so the hardware result is not tied to a modeled
frequency.

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
  direct+SIMD 1.70× faster in the latest pinned seven-trial median benchmark
  (122.7 vs 208.1 ns/message over 500,004 decoded messages).
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
- **Measurement**: serialized RDTSCP samples around parse+update per message,
  with timer overhead subtraction and logical-processor migration detection,
  bucketed into
  per-message-type histograms (p50/p95/p99/p99.9/p99.99). Numbers on this
  machine (AVX2 tier): all-message p50 203 ns, p99 527 ns, p99.9 671 ns;
  Trade `P` (no book op) 26 ns p50; Replace `U` (delete + insert) 367 ns p50
  and 613 ns p99. Raw CSVs and the machine-readable summary are regenerated by
  `run_all.sh`.
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
bottleneck at 100% occupancy, so extra buffering buys nothing for this
single-engine design. In the latest 20K-message run, ingest was stalled behind
the engine for 264,822 of 310,277 measured cycles (85.3%).

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
  software's 367 ns p50 in the current measurement).
- **Execute `E` carries no price** — the trade event is emitted at the
  order's *resting* price from the table lookup; `C` (execute-with-price)
  uses the message's price, gated on the printable flag; `P` passes through.
  These feed VWAP in `stats_engine` (also: spread, top-5 imbalance, depth).
- Costs: pure-Add stream commits every **8 cycles**; table-touching messages
  (E/X/D/U) take longer (probe + level update ± scan) → the 19-cycle p50 /
  27-cycle p99 service times on the mixed stream.

## 5. Verification

| layer | oracle | scale |
|---|---|---|
| SW hash table | `std::unordered_map` differential | 200k ops |
| SW book | `std::map` reference book, bit-for-bit | full streams |
| SW SIMD paths | scalar paths | per-routine |
| RTL modules | 11 directed/end-to-end Verilator executables (framer straddles, decoder fields, table collision/delete, replace, overlap, UDP/Mold stripping, banding, and full pipeline) | per-module and integrated |
| RTL end-to-end | C++ reference model, **full book state diffed after every message** | 20K-message replay |

143,796 checks on the software side in the current run. Both real bugs (§4.3,
§4.4) were
found by the reference diff, not by happy-path tests — the project's central
verification lesson.

## 6. The results, and how to talk about them

| measurement | result |
|---|---:|
| software parse + update | 203 ns p50, 527 ns p99, 671 ns p99.9 |
| book-only `std::map` | 208.1 ns/message |
| book-only direct-index/SIMD | 122.7 ns/message (1.70×) |
| RTL end-to-end | 55 cycles p50, 91 p99, 173 p99.9 |
| RTL service time | 19 cycles p50, 27 p99/p99.9 |
| RTL throughput at 100 MHz | 5.26 M messages/s from median service time |

The software book comparison is the median of seven alternating trials, not a
single run, and finishes with a state-equivalence check. RTL tails include
bounded best-price scans and are attributed by hardware counters. The
Spartan-7 build is constrained and routed at 100 MHz; Verilator supplies cycle
counts, while Vivado establishes whether the clock is implementable. See
`docs/results.json` for the generated workload, host, timing, and utilization
record.
