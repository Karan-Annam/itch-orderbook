# Board port — order book on real FPGA hardware

The host PC streams ITCH messages down a USB-UART and polls live book-state
snapshots back, while the full ingest → framer → decoder → **banded** book →
stats pipeline runs in fabric at 100 MHz. Two boards are supported:

| board  | part              | XDC              | reset button | notes |
|--------|-------------------|------------------|--------------|-------|
| urbana | xc7s50csga324-1   | `urbana.xdc`     | BTN0, active-**high** (`RESET_ACTIVE_HIGH`) | RealDigital Urbana |
| arty   | xc7a100tcsg324-1  | `arty_a7_100.xdc`| ck_rst, active-low | Digilent Arty A7-100 |

Urbana pins come from RealDigital's official master constraints file
(<https://www.realdigital.org/downloads/1f07b1146ec59e165634a5b6c782a17d.txt>,
fetched 2026-07-13): clock N15, `UART_TXD`=B16 (FPGA→PC), `UART_RXD`=A16
(PC→FPGA), LED0-3 = C13/C14/D14/D15, BTN0 = J2. The Arty pinout is reused
from `nmc_project/fpga/board/arty_a7_100.xdc`.

## Build & test

```bash
make sim-board                 # Verilator: exact board chain, no hardware
make bitstream BOARD=urbana    # full P&R -> build/fpga_top_urbana.bit
make bitstream BOARD=arty      #             build/fpga_top_arty.bit
```

The Verilator board test (`test_board.cpp`) drives real UART bit timing
through `fpga_top` (MMCM bypassed under `VERILATOR`) with the **SYNTHESIS**
(banded 1024-level) book configuration — the exact datapath the bitstream
ships. It covers snapshot polling, book building, checksum-corruption
recovery, and bad-length rejection.

## OBLink protocol (keep bridge RTL / test_board.cpp / ob_host.py in lockstep)

Host → FPGA, fixed **52 bytes**:

```
A5 5A | type | payload[48] | csum
type 0x01 ITCH_MSG : payload = one length-prefixed message ([2B BE len][body],
                     len 1..46), zero-padded to 48
type 0x02 SNAP_REQ : payload ignored
type 0x03 SET_BAND : payload[0..3] = band base u32 LE — pin the book's price
                     window to [base, base+1024) BEFORE streaming a symbol
                     (auto-centering on the first Add is fragile on real
                     feeds; tools/band_filter.py computes the base and
                     ob_host.py sends it from the file's sidecar)
csum = 8-bit sum of type + payload bytes
```

A checksum or length reject drops the frame whole (`frame_err_count`++,
sticky LED3) and the preamble hunt resynchronizes on the next frame — a
corrupted UART byte can never leak bytes into the message framer.

FPGA → host on each SNAP_REQ, fixed **132 bytes**: `A5 5A | 0x81 |
payload[128] | csum`. Payload fields (little-endian):

| off | type | field | off | type | field |
|-----|------|-------|-----|------|-------|
| 0 | u8 | version (=1) | 52 | u64 | trade_count |
| 1 | u8 | flags: b0 bid_valid, b1 ask_valid, b2 spread_valid | 60 | u32 | spread |
| 4 | u32 | best_bid_price | 64 | u32 | band_base |
| 8 | u32 | best_bid_shares | 68 | u32 | band_drops |
| 12 | u32 | best_ask_price | 72 | u64 | msg_total |
| 16 | u32 | best_ask_shares | 80 | u32×9 | mc: A F E C X D U P S |
| 20 | u64 | tot_bid_vol | 116 | u32 | frame_err_count |
| 28 | u64 | tot_ask_vol | 120 | u32 | tbl_ins_fails (nonzero = table undersized for churn) |
| 36 | u64 | vwap_num | | | |
| 44 | u64 | vwap_den | | | |

## Baud profiles

`UART_DIVISOR` = 100 MHz core clock / baud. Default **868** (115200, bring-up).
Demo: **100** (1 Mbaud, exact) — `make bitstream BOARD=urbana` then pass the
divisor as the second tclarg, or edit the Makefile call:
`-tclargs urbana 100`. Framed throughput at 1 Mbaud ≈ 1,900 msg/s — the UART
is the deliberate bottleneck (the core commits a message every ~10-60 cycles);
this is a demo link, not a market feed.

LEDs: 0 = heartbeat (~0.7 Hz), 1 = host frame accepted, 2 = snapshot sent,
3 = sticky attention (UART resync happened OR the band dropped an event).

## Bring-up ladder

1. Program the bitstream (Vivado HW manager). **LED0 heartbeats.**
2. `python tools/ob_host.py ping COM<n>` → an all-zero snapshot, LED2 blips.
   No response? Check baud matches the divisor, then TX/RX orientation.
3. `python tools/ob_host.py stream COM<n> <small.itch> --max 20` → snapshot
   shows `msg_total=20` and exact per-type counts.
4. Acceptance: `python tools/ob_host.py verify COM<n> data/real_sample_scaled.itch`
   → final snapshot equals the banded-sim replay of the same file
   (`tools/run_real_data.sh`), with `band_drops=0`, `frame_errs=0`.

Requires `pip install pyserial` on the host.
