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
- `rtl/`: synthesizable SystemVerilog, 16 bytes per clock, everything in SRAM

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

Sixteen bytes per cycle stream through the front end (the bus a 100G-class NIC
would realistically hand you). Byte-swapping costs nothing in hardware, you
just wire the bytes in the other order. The hash table and price levels are
SRAMs; a single book is instantiated (single symbol), and the Verilator
harness feeds it a single-symbol stream so the reference-model diff is exact.

### The wide framer: a compacting window instead of case-splitting

ITCH messages are length-prefixed and packed back-to-back with no alignment,
so on a 16-byte bus a message can start and end at any byte offset within a
word, and the 2-byte length prefix can straddle two words. Enumerating those
straddle cases as FSM states gets ugly fast. Instead `itch_framer` keeps a
32-byte *compacting window*: each cycle it consumes bytes from the front
(2 for a length prefix, up to 16 for a body beat) and appends the incoming
word at the back. A split length prefix stops being a special case — it's just
"window occupancy < 2, wait a cycle". Body bytes are re-aligned on the way
out, so the decoder always receives whole message-aligned 16-byte beats and
its big-endian field extraction stays pure wiring.

One property keeps this tractable: the minimum frame (2-byte prefix + 14-byte
System body) exactly equals the word size, so at most one message can end
inside any given word — the window never has to track more than "tail of the
current message + head of the next".

### Overlapping ingest with the book update

Originally the framer parked itself until the engine finished applying each
message, so a message's ingest and its book update were serialized. The
decoder already holds the decoded message in a register (`dec`) until the
engine accepts it, which is a free 1-deep buffer: the framer now only waits
when the *next* message is fully assembled and the engine still hasn't taken
the current one. Ingest of message N+1 (3-4 cycles) hides completely behind
the book update of N (8-20 cycles), and steady-state throughput is
engine-bound — a pure-Add stream commits every 8 cycles. A deeper FIFO would
add nothing: the engine is the bottleneck, and it can't run above 100%
occupancy. The invariant that makes it safe is that `msg_complete` is gated on
the decoder being empty, so `dec` is never overwritten while pending and
`dec_valid` always drops for at least one cycle between messages.

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

Hardware: with the original byte-serial front end, a mixed 20k-message stream
sustained one commit every ~50 cycles (200 ns at the modeled 250 MHz) — a
38-byte Add spent ~40 of those cycles just shifting bytes in, so software's
~190 ns median actually beat it. That measurement is what motivated the
current front end: a 16-byte-per-cycle ingest bus plus ingest/book-update
overlap. Same stream now: service p50 = 15 cycles (60 ns), p99 = 21 cycles
(84 ns), ~20.9M msg/s sustained — 4.2× the byte-serial throughput, and the
`ingest_stall_cycles` counter confirms the engine (not ingest) is the
bottleneck for 81% of the run. An Add arriving at an idle book commits ~60 ns
after its first byte.

So at the modeled clock the hardware now beats software's *median* while
keeping the property that was always its real advantage: the p99.9 service
time equals the p99 (no cache misses against fixed-latency SRAM, no OS
scheduler, no other processes competing for the core), while software's
p99.9 is ~7× its own median. That gap matters specifically because a real
feed's worst moments (bursts, volatility, everyone's software degrading at
once) are exactly when a fixed, boring latency number becomes a genuine edge
rather than a nice-to-have. Two honest caveats stand:

1. **250 MHz is a conservative, unsynthesized target,** not a
   post-place-and-route ceiling — but the wide framer's compare/shift logic is
   also shallow enough that it isn't the critical path; the async-read SRAMs
   are (see Limitations).
2. **The CPU still wins on raw clock.** A 4-5 GHz out-of-order core is
   extremely good at hot, predictable, cache-resident integer work; the
   hardware wins by doing less per message (no instruction stream at all),
   not by switching faster.

The remaining hardware tail is the occasional best-price scan after the top
level empties — bounded, deterministic, and attributed cycle-by-cycle in the
perf counters rather than being a scheduler surprise.

## Limitations

- RTL is single-symbol; the software book handles all symbols. Multi-symbol
  hardware needs per-symbol book banks or a book-state cache.
- The RTL memories use combinational (async) reads. Valid LUTRAM, but a real
  FPGA build should re-map them to registered BRAM with an extra pipeline stage.
- No live UDP test on this machine (no feed, no libxdp); the receiver design
  follows the standard AF_XDP pattern but is exercised only via file replay.
