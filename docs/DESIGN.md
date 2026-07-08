# Design notes

How the project fits together, and the two hardware bugs that taught me the most.
Build/run instructions are in [BUILDING.md](BUILDING.md).

## The idea

An exchange order book is the canonical low-latency problem: a stream of small
binary messages (adds, cancels, executes, replaces), each of which must update a
data structure in as little time as possible. HFT firms attack it two ways:
aggressively tuned C++ on a CPU, or an FPGA. I wanted to build both against the
same feed and measure the actual gap, not just repeat the folklore about it.

So there are two implementations of the same book:

- `sw/`: C++17, SIMD everywhere it pays, kernel-bypass-style receiver design
- `rtl/`: synthesizable SystemVerilog, one byte per clock, everything in SRAM

Both consume the same NASDAQ ITCH 5.0 byte stream and must produce identical
book state. The RTL is diffed against a `std::map` reference model message by
message, which is also how both of the real bugs below were caught.

## The wire format contract

ITCH 5.0 is a length-prefixed binary format, big-endian everything. The message
layouts live in exactly one place per language (`sw/parser/itch_messages.hpp`
and `rtl/ob_pkg.sv`), and the two mirror each other field for field. If they
ever disagree, the end-to-end diff fails immediately, so the contract is
enforced by test rather than by discipline.

One wrinkle: this project uses an 8-byte timestamp while real ITCH 5.0 uses 6
bytes on the wire. Software, RTL, and the synthetic generator all agree with
each other, so everything here is self-consistent; feeding in a real NASDAQ
file needs the 6→8 adjustment described in `data/README.md`.

## Software side

The pipeline: receiver → SPSC ring buffer → parser → order-ref hash table →
book → stats.

**Receivers.** The interesting ones (AF_XDP zero-copy, raw socket + busy poll)
are Linux-only and `#ifdef`-guarded; the portable default replays a file. The
hot path downstream is identical either way, so latency numbers don't depend on
which receiver feeds it.

**Order-ref hash table** (`sw/book/order_ref_table.hpp`). Execute/Cancel/Delete/
Replace messages identify an order only by a 64-bit reference number, so you
need ref → (price, side, shares) at O(1). Open addressing with linear probing,
Thomas Wang's mix64 as the hash. Two decisions worth defending:

- *Structure-of-arrays layout.* Keys live in their own contiguous array, so the
  SIMD probe is one aligned load of 8 (AVX-512) or 4 (AVX2) consecutive keys,
  no gathers. Values sit in parallel arrays indexed by slot.
- *Backward-shift deletion* instead of tombstones. On erase, later entries in
  the probe chain shift back to fill the hole. Chains never accumulate dead
  slots, so lookup cost doesn't degrade over a long session. A 200k-operation
  differential test against `std::unordered_map` keeps it honest.

**The book** (`sw/book/order_book.hpp`). Price levels are a direct-indexed
array where price *is* the index, so an update is a load, an add, a store. Best
bid/ask are maintained incrementally; the only expensive case is deleting the
last shares at the best price, which triggers a vectorized scan toward the next
non-empty level. `std::map` (the original book, kept as `reference_book.hpp`)
does the same job with a red-black tree; the bench shows the direct+SIMD book is
roughly 2× faster on the same stream (1.5–2.6× run to run).

**Measurement.** RDTSC around parse+update per message, bucketed into a
histogram per message type (p50/p95/p99/p99.9/p99.99). Trade messages don't
touch the book and clock in around 35 ns; Deletes and Replaces are the most
expensive, exactly as you'd expect (rescans and sequential sub-operations).

## Hardware side

```
udp_stripper → itch_framer → itch_decoder → order_ref_table
                                              → book_update_engine (+ best_tracker ×2)
                                              → stats_engine, perf_counters
```

One byte per cycle streams through the front end. Byte-swapping costs nothing in
hardware, you just wire the bytes in the other order. The hash table and price
levels are SRAMs; a single book is instantiated (single symbol), and the
Verilator harness feeds it a single-symbol stream so the reference-model diff is
exact.

### Bug 1: deleting from a linear-probe table by clearing the slot

My first hash-table delete wrote the slot back to empty. That's wrong with
linear probing: an emptied slot terminates every probe chain that runs through
it, so any colliding key stored *after* it becomes unreachable. The software
side dodges this with backward-shift deletion, but shifting SRAM contents around
in hardware is miserable. The fix is tombstones: a deleted slot gets a `tomb`
mark that lookups probe straight past (only truly-empty stops a search) and that
inserts are allowed to reuse. A test that forces a 24-deep collision chain and
then deletes out of the middle of it is what caught this.

Tombstones aren't reclaimed in bulk, so an extremely long session would want a
sweep/rehash eventually, fine at a 64K table for these workloads.

### Bug 2: the best-tracker handshake race

When a delete empties the best price level, the engine kicks the best tracker to
scan for the next non-empty level, and waited for `!scanning` to declare the
message done. But the tracker raises `scanning` one cycle *after* it samples the
kick. In that window the engine could commit with `best_price` still pointing at
the just-emptied level, a stale, zero-share best. Classic ready/busy handshake
race. The fix (in `book_update_engine.sv`) computes whether a scan is *expected*
for this message and waits to see it actually start and finish
(`expect_scan_q` / `scan_seen_q`). `test_book_update` now pins the invariant
that the best price always has shares.

Both bugs are the kind that only surface when you diff hardware against a
reference model on thousands of messages. Neither showed up in happy-path
tests.

## What the comparison actually shows

Software (this machine, AVX2 tier): p50 around 190 ns per message, p99 around
540 ns, p99.9 around 1.3 µs. The tail is cache misses and OS jitter, nothing
in the code path varies by 10× on its own.

Hardware: ~86 cycles for an Add, so ~344 ns at the 250 MHz the pipeline is
modeled at, and it's the *same* number every time.

Worth being honest about what's actually being compared here: software's
*median* (~190 ns) is lower than hardware's number. That's a real result, not
a bug, and it has three concrete causes, not just "software happened to be
fast":

1. **Clock speed.** The CPU runs at 4-5 GHz; the pipeline here is modeled at
   250 MHz, roughly 16-20× slower. A superscalar, out-of-order x86 core is
   also just very good at hot, predictable, cache-resident integer work
   (parsing, hashing, array indexing), which is exactly what this hot path is.
2. **The front end is byte-serial.** `udp_stripper → itch_framer →
   itch_decoder` accepts one byte per cycle, so a 38-byte Add message spends
   roughly half of its ~86 total cycles just shifting bytes in before any book
   logic runs at all. That's a real architectural cost of choosing a simple,
   easy-to-verify one-byte-wide datapath over a wider parallel ingest path.
   A design reading 8 or 16 bytes/cycle (realistic off a 100G+ NIC) would cut
   this component sharply.
3. **250 MHz is a conservative, unsynthesized target,** not a
   post-place-and-route ceiling. It was chosen as a number the design would
   obviously close timing at without backend work. Real FPGA order-book
   implementations commonly clock past 300-500 MHz; an ASIC would go higher
   still. Combined with a wider front end, closing most or all of the
   mean-latency gap is realistic, just not something this project measured.

So the honest framing is: software's best case is genuinely faster than
hardware's only case, for reasons above, not despite them. What hardware
actually buys is not a lower mean, it's that the p99.9 *equals* the p50 (no
cache misses are possible against SRAM with a fixed access latency, no OS
scheduler, no other processes competing for the core), while software's
p99.9 is ~7× its own median. That gap matters specifically because a real
feed's worst moments (bursts, volatility, everyone's software degrading at
once) are exactly when a fixed, boring latency number becomes a genuine edge
rather than a nice-to-have. (Caveat, restated: 250 MHz is a modeled clock,
not post-place-and-route timing.)

## Limitations

- RTL is single-symbol; the software book handles all symbols. Multi-symbol
  hardware needs per-symbol book banks or a book-state cache.
- The RTL memories use combinational (async) reads. Valid LUTRAM, but a real
  FPGA build should re-map them to registered BRAM with an extra pipeline stage.
- No live UDP test on this machine (no feed, no libxdp); the receiver design
  follows the standard AF_XDP pattern but is exercised only via file replay.
