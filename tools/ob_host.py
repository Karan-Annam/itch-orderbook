#!/usr/bin/env python3
"""ob_host.py — host-side OBLink client for the order-book FPGA board.

Speaks the framing defined in fpga/board/uart_itch_bridge.sv (keep the three
implementations — bridge RTL, fpga/board/test_board.cpp, this file — in
lockstep):

  host -> FPGA, 52 bytes:  A5 5A | type | payload[48] | csum(type+payload)
      type 0x01 ITCH_MSG  payload = one length-prefixed message, zero-padded
      type 0x02 SNAP_REQ
  FPGA -> host, 132 bytes: A5 5A | 0x81 | payload[128] LE | csum

Subcommands:
  selftest             codec golden-vector checks (no hardware needed)
  ping PORT            request one snapshot and pretty-print it
  stream PORT FILE     stream a length-prefixed .itch file, live book display
  verify PORT FILE     stream, then diff the final snapshot against
                       `orderbook_sw --final-state` semantics (prints fields
                       for manual/scripted comparison)

Options: --baud (default 115200; use 1000000 with a divisor-100 bitstream),
--poll-hz N (stream display refresh), --max N (limit messages).
"""
import argparse
import struct
import sys
import time

PREAMBLE = b"\xa5\x5a"
T_ITCH, T_SNAP, T_BAND, T_SNAPRESP = 0x01, 0x02, 0x03, 0x81
PAYLOAD_DN, SNAP_BYTES = 48, 128
FRAME_DN = 2 + 1 + PAYLOAD_DN + 1          # 52
FRAME_UP = 2 + 1 + SNAP_BYTES + 1          # 132

MC_NAMES = ["Add", "AddMPID", "Exec", "ExecPr", "Cancel", "Delete",
            "Replace", "Trade", "System"]


def csum8(data: bytes) -> int:
    return sum(data) & 0xFF


def frame_itch(msg: bytes) -> bytes:
    """Wrap one length-prefixed ITCH message ([2B BE len][body]) in an OBLink frame."""
    if not (3 <= len(msg) <= PAYLOAD_DN):
        raise ValueError(f"message must be 3..{PAYLOAD_DN} bytes, got {len(msg)}")
    blen = struct.unpack(">H", msg[:2])[0]
    if blen != len(msg) - 2:
        raise ValueError(f"length prefix {blen} != body bytes {len(msg) - 2}")
    pay = msg.ljust(PAYLOAD_DN, b"\x00")
    body = bytes([T_ITCH]) + pay
    return PREAMBLE + body + bytes([csum8(body)])


def frame_snap_req() -> bytes:
    body = bytes([T_SNAP]) + b"\x00" * PAYLOAD_DN
    return PREAMBLE + body + bytes([csum8(body)])


def frame_set_band(base: int) -> bytes:
    """Pin the book's price window to [base, base+1024) before streaming."""
    pay = struct.pack("<I", base).ljust(PAYLOAD_DN, b"\x00")
    body = bytes([T_BAND]) + pay
    return PREAMBLE + body + bytes([csum8(body)])


def sidecar_band(path: str) -> int | None:
    """band_base from the file's .meta.json sidecar (tools/band_filter.py)."""
    import json, os
    meta = path + ".meta.json"
    if os.path.exists(meta):
        with open(meta) as f:
            return json.load(f).get("band_base")
    return None


def parse_snapshot(payload: bytes) -> dict:
    assert len(payload) == SNAP_BYTES
    u32 = lambda o: struct.unpack_from("<I", payload, o)[0]
    u64 = lambda o: struct.unpack_from("<Q", payload, o)[0]
    flags = payload[1]
    return {
        "version": payload[0],
        "bid_valid": bool(flags & 1), "ask_valid": bool(flags & 2),
        "spread_valid": bool(flags & 4),
        "bid_px": u32(4), "bid_sh": u32(8), "ask_px": u32(12), "ask_sh": u32(16),
        "tot_bid": u64(20), "tot_ask": u64(28),
        "vwap_num": u64(36), "vwap_den": u64(44), "trades": u64(52),
        "spread": u32(60), "band_base": u32(64), "band_drops": u32(68),
        "msg_total": u64(72),
        "mc": [u32(80 + 4 * i) for i in range(9)],
        "frame_errs": u32(116),
        "tbl_ins_fails": u32(120),
    }


def read_snapshot(ser, timeout_s: float = 2.0) -> dict | None:
    """Hunt the upstream byte stream for one valid snapshot frame."""
    deadline = time.monotonic() + timeout_s
    state, buf, ftype = 0, bytearray(), 0
    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue
        v = b[0]
        if state == 0:
            state = 1 if v == 0xA5 else 0
        elif state == 1:
            state = 2 if v == 0x5A else (1 if v == 0xA5 else 0)
        elif state == 2:
            ftype, buf, state = v, bytearray(), 3
        else:
            buf.append(v)
            if len(buf) == SNAP_BYTES + 1:
                ok = (ftype == T_SNAPRESP and
                      buf[SNAP_BYTES] == csum8(bytes([ftype]) + bytes(buf[:SNAP_BYTES])))
                if ok:
                    return parse_snapshot(bytes(buf[:SNAP_BYTES]))
                state = 0
    return None


def print_snapshot(s: dict) -> None:
    d = lambda px: px / 10000.0
    print(f"  msg_total={s['msg_total']}  frame_errs={s['frame_errs']}  "
          f"band_base={s['band_base']}  band_drops={s['band_drops']}  "
          f"tbl_ins_fails={s['tbl_ins_fails']}")
    bid = f"{s['bid_px']} (${d(s['bid_px']):.4f}) x {s['bid_sh']}" if s["bid_valid"] else "-"
    ask = f"{s['ask_px']} (${d(s['ask_px']):.4f}) x {s['ask_sh']}" if s["ask_valid"] else "-"
    print(f"  best bid: {bid}\n  best ask: {ask}")
    if s["spread_valid"]:
        print(f"  spread:   {s['spread']} ticks")
    if s["vwap_den"]:
        print(f"  vwap:     {s['vwap_num'] / s['vwap_den']:.2f} ticks over "
              f"{s['trades']} trades")
    print(f"  vols:     bid {s['tot_bid']}  ask {s['tot_ask']}")
    print("  counts:   " + "  ".join(f"{n}={c}" for n, c in zip(MC_NAMES, s["mc"]) if c))


def iter_messages(path: str, max_n: int = 0):
    """Yield length-prefixed messages from a project-format .itch file."""
    with open(path, "rb") as f:
        data = f.read()
    o, n = 0, 0
    while o + 2 <= len(data):
        blen = struct.unpack_from(">H", data, o)[0]
        if blen == 0 or o + 2 + blen > len(data):
            break
        yield data[o:o + 2 + blen]
        o += 2 + blen
        n += 1
        if max_n and n >= max_n:
            break


def open_serial(port: str, baud: int):
    import serial  # lazy: selftest must run without pyserial
    return serial.serial_for_url(port, baudrate=baud, timeout=0.1)


# ---- subcommands -------------------------------------------------------------

def cmd_selftest(_args) -> int:
    fails = 0

    def chk(cond, msg):
        nonlocal fails
        if not cond:
            print(f"  [FAIL] {msg}")
            fails += 1

    # golden ITCH frame: the same del(2) message test_board.cpp sends is
    # 2-byte prefix + 27-byte Delete body; spot-check framing invariants
    msg = struct.pack(">H", 27) + b"D" + b"\x00" * 26
    f = frame_itch(msg)
    chk(len(f) == FRAME_DN, "downstream frame is 52 bytes")
    chk(f[:2] == PREAMBLE and f[2] == T_ITCH, "preamble+type")
    chk(f[3:3 + 29] == msg and all(b == 0 for b in f[32:51]), "payload+padding")
    chk(f[51] == csum8(f[2:51]), "checksum covers type+payload")

    sf = frame_snap_req()
    chk(len(sf) == FRAME_DN and sf[2] == T_SNAP, "snap request frame")
    chk(sf[51] == 0x02, "snap request csum is just the type byte")

    bf = frame_set_band(28359)
    chk(len(bf) == FRAME_DN and bf[2] == T_BAND, "set-band frame")
    chk(bf[3:7] == struct.pack("<I", 28359), "set-band base LE")
    chk(bf[51] == csum8(bf[2:51]), "set-band csum")

    # snapshot round-trip: build a payload, parse it back
    pay = bytearray(SNAP_BYTES)
    pay[0] = 1; pay[1] = 0x7
    struct.pack_into("<I", pay, 4, 600000); struct.pack_into("<I", pay, 8, 70)
    struct.pack_into("<Q", pay, 36, 600000 * 30); struct.pack_into("<Q", pay, 44, 30)
    struct.pack_into("<Q", pay, 72, 3); struct.pack_into("<I", pay, 80, 2)
    s = parse_snapshot(bytes(pay))
    chk(s["bid_valid"] and s["bid_px"] == 600000 and s["bid_sh"] == 70, "snapshot fields")
    chk(s["vwap_num"] == 600000 * 30 and s["msg_total"] == 3 and s["mc"][0] == 2,
        "snapshot 64-bit fields")

    # loopback smoke through pyserial if available
    try:
        ser = open_serial("loop://", 115200)
        ser.write(frame_snap_req())
        echoed = ser.read(FRAME_DN)
        chk(echoed == frame_snap_req(), "loop:// echoes the frame")
    except ImportError:
        print("  [skip] pyserial not installed; loop:// smoke skipped")

    print(f"[selftest] {'PASS' if fails == 0 else 'FAIL'} ({fails} failures)")
    return 1 if fails else 0


def cmd_ping(args) -> int:
    ser = open_serial(args.port, args.baud)
    ser.reset_input_buffer()
    ser.write(frame_snap_req())
    s = read_snapshot(ser)
    if s is None:
        print("[ping] no snapshot response (check port, baud/divisor, TX/RX wiring)")
        return 1
    print(f"[ping] snapshot v{s['version']} from {args.port} @ {args.baud} baud")
    print_snapshot(s)
    return 0


def cmd_stream(args, verify: bool = False) -> int:
    ser = open_serial(args.port, args.baud)
    ser.reset_input_buffer()
    band = args.band if args.band is not None else sidecar_band(args.file)
    if band is not None:
        ser.write(frame_set_band(int(band)))
        print(f"[stream] band pinned to [{band}, {int(band) + 1024})")
    sent, t0 = 0, time.monotonic()
    next_poll = t0
    for msg in iter_messages(args.file, args.max):
        ser.write(frame_itch(msg))
        sent += 1
        now = time.monotonic()
        if args.poll_hz > 0 and now >= next_poll:
            next_poll = now + 1.0 / args.poll_hz
            ser.write(frame_snap_req())
            s = read_snapshot(ser, timeout_s=1.0)
            if s:
                rate = sent / max(now - t0, 1e-9)
                print(f"\x1b[2J\x1b[H[stream] {sent} msgs sent  ({rate:,.0f} msg/s)")
                print_snapshot(s)
    ser.flush()
    time.sleep(0.2)
    ser.write(frame_snap_req())
    s = read_snapshot(ser)
    print(f"\n[stream] done: {sent} messages in {time.monotonic() - t0:.1f}s")
    if s is None:
        print("[stream] WARNING: no final snapshot")
        return 1
    print_snapshot(s)
    if verify:
        ok = (s["msg_total"] == sent and s["frame_errs"] == 0 and
              s["band_drops"] == 0 and s["tbl_ins_fails"] == 0)
        print(f"[verify] msg_total==sent: {s['msg_total'] == sent}  "
              f"frame_errs==0: {s['frame_errs'] == 0}  "
              f"band_drops==0: {s['band_drops'] == 0}  "
              f"tbl_ins_fails==0: {s['tbl_ins_fails'] == 0}")
        print("[verify] compare book fields against: "
              f"build/orderbook_sw {args.file} --final-state")
        return 0 if ok else 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest")
    for name in ("ping", "stream", "verify"):
        p = sub.add_parser(name)
        p.add_argument("port", help="COM port (e.g. COM5) or pyserial URL")
        if name != "ping":
            p.add_argument("file", help="project-format .itch file")
            p.add_argument("--poll-hz", type=float, default=5.0)
            p.add_argument("--max", type=int, default=0)
            p.add_argument("--band", type=int, default=None,
                           help="pin band base (default: file's .meta.json sidecar)")
        p.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()
    if args.cmd == "selftest":
        return cmd_selftest(args)
    if args.cmd == "ping":
        return cmd_ping(args)
    return cmd_stream(args, verify=(args.cmd == "verify"))


if __name__ == "__main__":
    sys.exit(main())
