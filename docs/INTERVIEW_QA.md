# Interview Q&A — Order Book (C++ + RTL)

Rehearsal deck. Every answer: **decision → reason → evidence**.

---

## Pitches

**30s:** "I built the same NASDAQ ITCH 5.0 order book twice — SIMD-tuned C++
and synthesizable SystemVerilog — fed both the identical byte stream, and
diffed the RTL's full book state against a reference model on every message.
Software does ~190 ns per message at p50 but its p99.9 is ~7× worse; the
hardware, after I redesigned the front end to 16 bytes/cycle with
ingest/update overlap, services a message every 60 ns with a p99.9 *equal* to
its p99. The reference diff also caught two genuine RTL bugs — a hash-table
delete that orphaned collision chains and a one-cycle handshake race in the
best-price tracker."

**Add for 2 min:** the compacting-window framer (unaligned back-to-back
messages on a wide bus without case explosion), the deliberate divergence in
deletion strategy (backward-shift in software vs tombstones in hardware, and
why each fits its medium), and the v1 humility story — the first hardware was
byte-serial and *lost* to software's median, and measuring that is what drove
the 4.2× front-end redesign.

---

## Data structures / C++

**Q: Why a hash table for order refs and an array for prices?**
Two different key distributions. Order refs are 64-bit, effectively random,
unbounded → hash table. Prices are bounded, dense near top-of-book, and
locality matters → direct-indexed array where price *is* the index, so an
update is load-add-store and the common case never chases a pointer. The
`std::map` book is kept as the oracle and baseline — the direct+SIMD book is
~2× faster on the same stream (~90–110 vs ~200–235 ns/msg book-only).

**Q: Walk me through your hash table.**
Open addressing, linear probing, 2²⁰ slots, load < 0.7, key 0 as the empty
sentinel. Wang mix64 hash — the feed's reference numbers are nearly
sequential, which is the adversarial case for weak hashes. Structure-of-arrays
so 8 keys share a cache line; the probe is one AVX-512 (or two AVX2) compare
over the whole line — no gathers. Deletion is backward-shift (Knuth 6.4-R),
so chains never accumulate dead slots; the subtle part is the cyclic
"is its home slot in the gap" test with wrap-around. Verified by a 200k-op
differential test against `std::unordered_map`.

**Q: Why backward-shift in software but tombstones in hardware?**
Cost model inversion. In software, shifting a few cache-resident entries is
cheap and keeps probes short forever. In hardware, each shift is an SRAM
read-then-write per cycle down the chain — variable, long, and stateful; a
tombstone is a 1-bit write. The RTL insert also reuses the *first* tombstone
seen on its probe path, which bounds chain growth. Same abstract structure,
different deletion algorithm per medium — that's the point of building both.

**Q: How do you measure nanosecond-scale latency credibly?**
RDTSC around parse+update per message (invariant TSC), histogrammed
per message type up to p99.99, 1M-message runs, pinned core. And I report the
distribution, not the mean — the entire conclusion of the project lives in
the difference between p50 and p99.9.

**Q: What's an SPSC ring and why here?**
Single-producer/single-consumer lock-free queue between receiver and parser —
power-of-two capacity, masked indices, acquire/release ordering, no CAS
needed because each index has one writer. It decouples receive jitter from
the parse hot path.

## RTL / microarchitecture

**Q: How do you parse an unaligned byte-packed protocol on a 16-byte bus?**
A 32-byte compacting window instead of case-splitting: consume 2 bytes for a
length prefix or up to 16 for body from the front, append the new word at the
back. A length prefix straddling two words is just "occupancy < 2, wait."
The bound that makes 32 B sufficient: the minimum frame (16 B) equals the bus
width, so at most one message boundary lands per word — the window only ever
holds "tail of current + head of next."

**Q: Where did the 4.2× come from?**
v1 was byte-serial: a 38-byte Add spent ~40 of ~50 cycles shifting bytes in —
ingest-bound, and software beat it. Two changes: the 16 B/cycle framer
(ingest of an Add drops to 3–4 cycles) and overlapping ingest of message N+1
with the book update of N, using the decoder's existing output register as a
free 1-deep skid buffer. Now the *engine* is the bottleneck (81% of cycles by
counter), pure Adds commit every 8 cycles, and the mixed stream sustains
20.9 M msg/s. I didn't add a deeper FIFO because a saturated engine can't
exceed 100% occupancy — buffering would add latency, not throughput.

**Q: Explain the handshake race you hit.**
The engine kicked the best-tracker's scan and waited for `!scanning` — but
the tracker asserts `scanning` one cycle after sampling the kick, so the
engine could sample the *old* idle state and commit a stale best price
pointing at the just-emptied level. Fix: the engine computes whether a scan
is expected and requires seeing it start and finish (`expect_scan_q`/
`scan_seen_q`). General lesson: never wait on a busy flag you didn't verify
went high — either register the request-accepted event or use a
credit/valid-ready handshake with no combinational sampling window.

**Q: Why is deleting the best price the expensive case in both worlds?**
Emptying the best level forces a search for the next non-empty level. In
software that's a vectorized linear scan; in hardware a 1-level-per-cycle SRAM
scan. Both are bounded by price density near top-of-book. The hardware
tracker also has the O(1) escape: per-side order counters tell it when the
side is fully empty so it never scans a dead side. It's the one deterministic
"tail" hardware has — and it's visible in perf counters, unlike a cache miss.

**Q: Your SRAMs are async-read. Is that synthesizable?**
Yes as LUTRAM (distributed RAM) — but a 2²⁰-entry level array belongs in
block RAM, which is synchronous-read; the honest FPGA port adds a registered
read stage (+1 cycle on table/level accesses) and re-times the FSMs. That and
the unsynthesized 250 MHz target are stated limitations, not fine print.

**Q: How would you make the RTL multi-symbol?**
The book state (level SRAMs, trackers, counters) is per-symbol; the hash
table is already keyed by ref. Options: bank the book per symbol (area-heavy,
constant time), or treat books as a cache keyed by locate (like real
feed-handler FPGAs: hot symbols resident, cold in DRAM). The decoder/framer
front end is symbol-agnostic and unchanged either way.

## Verification

**Q: How do you know hardware and software agree?**
One wire contract (`itch_messages.hpp` ↔ `ob_pkg.sv`, mirrored field for
field), one synthetic generator producing byte-exact streams, and a
reference-model diff of *full book state after every message* on 20k+ message
replays — plus directed module tests (framer straddle patterns, a forced
24-deep collision chain with delete-from-the-middle, replace semantics,
overlap timing). Both real bugs were invisible to happy-path tests and caught
only by the exhaustive diff.

**Q: What would you check before trusting this at an exchange?**
Real ITCH pcaps (6-byte timestamps, MoldUDP64 framing, session events),
symbol universe scale, gap/retransmission handling, post-P&R timing closure
at the claimed clock, BRAM-mapped memories, and a hardware-in-the-loop replay
against the same reference model.

## Attack questions

**"250 MHz is made up."** It's a modeled clock, and the docs say so. The
cycle counts are the measurement; 250 MHz is a conservative conversion chosen
to be defensible for this pipeline's shallow logic on a mid-range FPGA. Even
at 150 MHz the p50 is 100 ns and the tail story (p99.9 = p99) is untouched —
determinism is clock-independent.

**"Software could be faster."** Yes — this is AVX2 on a laptop with a
file-replay receiver; kernel-bypass NIC, hugepages, hotter tuning all help
the median. But nothing in software fixes the tail: the ~7× p50→p99.9 blowup
is cache misses and scheduling, which are architectural. The hardware
argument was never "faster median" (v1 in fact lost it); it's the flat tail
during exactly the bursts when software degrades.

**"Trade P at 35 ns proves parsing is trivial — so what's the 190 ns?"**
Exactly the right read: parse is cheap; the book and hash-table touches are
the cost. That's why the engine (not ingest) bounds the hardware too, and why
Replace — two table ops plus two level ops — is the worst message in both
implementations. The symmetry is evidence the comparison is apples-to-apples.

**"Why not just use a std::unordered_map and move on?"** It's in the repo —
as the differential oracle. On the hot path it loses on all three axes:
chaining pointer-chases per probe, allocation on insert, and no SIMD-able
layout. The custom table is the difference between ~190 ns and considerably
worse; more importantly the *pair* (fast structure + slow oracle) is the
methodology.

---

## Numbers to memorize

- SW: p50 **190 ns**, p99 540 ns, p99.9 **1.3 µs** (~7× median); Trade 35 ns,
  Replace 260/650 ns. Direct+SIMD book ≈ **2×** vs `std::map`.
- HW v1 byte-serial: 50 cyc / 200 ns p50, ~5 M msg/s. HW v2: **15 cyc /
  60 ns p50, 21 cyc / 84 ns p99 = p99.9, 20.9 M msg/s (4.2×)**; 20k messages:
  1,006,092 → 239,458 cycles; engine-bound 81% (ingest_stall counter).
- Pure-Add commit interval: 8 cycles. Ingest of an Add: 3–4 cycles (hidden).
- Tables: 64K RTL slots (XOR-fold hash, MAX_PROBE 64, tombstones) vs 2²⁰ SW
  slots (Wang mix64, backward-shift). Price levels: 2²⁰ direct-indexed.
- Verification: ~140k SW assertions; 200k-op hash differential; 20k+ message
  RTL reference diff; 2 real bugs found by the diff.
