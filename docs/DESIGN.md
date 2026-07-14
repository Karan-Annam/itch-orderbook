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
does the same job with a red-black tree; the latest seven-trial median benchmark
measures the direct+SIMD book at 1.70× the baseline throughput.

**Measurement.** RDTSC around parse+update per message, bucketed into a
histogram per message type (p50/p95/p99/p99.9/p99.99). Trade messages don't
touch the book and clock in around 26 ns; Deletes and Replaces are the most
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

Software (this machine, AVX2 tier): serialized TSC measurement over parse plus
book update gives 203 ns p50, 527 ns p99, and 671 ns p99.9. The harness
subtracts timer overhead and drops samples that migrate logical processors.
The book-only benchmark separately reports the median of seven alternating,
pinned trials: 208.1 ns/message for `std::map` and 122.7 ns/message for the
direct-index/SIMD engine (1.70×), followed by a final-state equality check.

Hardware uses a 16-byte-per-cycle ingest bus with ingest/book-update overlap.
On the same 20k-message workload, end-to-end latency (first input byte through
commit) is 55 cycles p50, 91 p99, and 173 p99.9. Commit-to-commit service time
is 19 cycles p50 and 27 cycles at both p99 and p99.9. At the implemented
100 MHz Spartan-7 clock those values are 550 ns, 910 ns, 1.73 µs and a median
service rate of 5.26 M messages/s. The cycle counts are the portable result;
nanoseconds are derived only from the implemented clock.

The remaining hardware tail is the occasional best-price scan after the top
level empties — bounded, deterministic, and attributed cycle-by-cycle in the
perf counters rather than being a scheduler surprise.

## Implementation scope

- RTL is single-symbol; the software book handles all symbols. Multi-symbol
  hardware needs per-symbol book banks or a book-state cache.
- The FPGA configuration uses a price-banded BRAM implementation; the larger
  simulation configuration is not intended to map directly to this device.
- No live UDP test on this machine (no feed, no libxdp); the receiver design
  follows the standard AF_XDP pattern but is exercised only via file replay.
