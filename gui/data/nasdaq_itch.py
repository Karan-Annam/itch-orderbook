"""Ingest real NASDAQ TotalView-ITCH 5.0 files.

Real ITCH 5.0 uses 6-byte (48-bit) nanosecond timestamps; this project uses
8-byte timestamps internally. This module reads the real wire format, keeps the
9 message types we model (A F E C X D U P S), and re-emits them in project
format via itch_writer.py — the only change is the 2-byte timestamp zero-pad
(the remaining field layout is identical but shifted by +2).

Real ITCH 5.0 body layouts (from NASDAQ TotalView-ITCH 5.0 spec):
  Header 11 bytes: type[1] + locate[2] + tracking[2] + ts48[6]
  A: 36 bytes  (hdr + ref[8] + side[1] + shares[4] + stock[8] + price[4])
  F: 40 bytes  (A + mpid[4])
  E: 31 bytes  (hdr + ref[8] + shares[4] + match[8])
  C: 36 bytes  (hdr + ref[8] + shares[4] + match[8] + printable[1] + price[4])
  X: 23 bytes  (hdr + ref[8] + shares[4])
  D: 19 bytes  (hdr + ref[8])
  U: 35 bytes  (hdr + orig[8] + new[8] + shares[4] + price[4])
  P: 44 bytes  (hdr + ref[8] + side[1] + shares[4] + stock[8] + price[4] + match[8])
  S: 12 bytes  (hdr + event_code[1])
"""
from __future__ import annotations
import gzip
import io
import os
import struct
import sys
from pathlib import Path
from typing import Iterator

from .itch_writer import (
    ItchWriter, system_event, add_order, add_order_mpid, order_executed,
    order_executed_price, order_cancel, order_delete, order_replace, trade,
    _frame
)

# ---- real ITCH 5.0 header (11 bytes) ---------------------------------------
_HDR_FMT = '>cHH'  # type(1) + locate(2) + tracking(2) = 5 bytes + 6 bytes ts48
_HDR_LEN = 11


def _read_u16(b: bytes, off: int) -> int:
    return struct.unpack_from('>H', b, off)[0]

def _read_u32(b: bytes, off: int) -> int:
    return struct.unpack_from('>I', b, off)[0]

def _read_u64(b: bytes, off: int) -> int:
    return struct.unpack_from('>Q', b, off)[0]

def _read_ts48(b: bytes, off: int) -> int:
    """Read 6-byte big-endian integer (ns since midnight)."""
    hi = struct.unpack_from('>H', b, off)[0]
    lo = struct.unpack_from('>I', b, off + 2)[0]
    return (hi << 32) | lo

def _read_stock(b: bytes, off: int) -> str:
    return b[off:off + 8].decode('ascii', errors='replace').strip()


# ---- convert one real-ITCH message body → project-format bytes --------------
# Real fields start at offset 11 (after 11-byte header).
# Project fields start at offset 13 (after 13-byte header).
# We just re-emit via itch_writer functions (which build the 13-byte header).

def _convert(body: bytes, ts48: int, locate: int, tracking: int,
             tick_scale: int = 1) -> bytes | None:
    """
    Convert a real ITCH body (11-byte header + type-specific payload) to project
    format. Returns length-prefixed project bytes, or None for unsupported types.
    ts48 is already extracted from the header.

    tick_scale integer-divides every price field. Real ITCH prices are in
    1/10000-dollar units; tick_scale=100 re-grids to cents, which puts $100-500
    symbols at 10k-50k ticks — inside the RTL's banded FPGA window (1024 ticks
    = $10.24 of range) as long as the symbol moves less than that in a day.
    """
    if not body:
        return None
    mtype = chr(body[0])
    # Type-specific fields are at offset 11 in real ITCH (right after the real header)
    p = 11  # payload start in real body
    scale = max(1, int(tick_scale))

    try:
        if mtype == 'A':
            oref   = _read_u64(body, p);     p += 8
            side   = chr(body[p]);           p += 1
            shares = _read_u32(body, p);     p += 4
            stock  = _read_stock(body, p);   p += 8
            price  = _read_u32(body, p) // scale
            return add_order(locate=locate, tracking=tracking, timestamp_ns=ts48,
                             order_ref=oref, side=side, shares=shares,
                             stock=stock, price=price)

        elif mtype == 'F':
            oref   = _read_u64(body, p);     p += 8
            side   = chr(body[p]);           p += 1
            shares = _read_u32(body, p);     p += 4
            stock  = _read_stock(body, p);   p += 8
            price  = _read_u32(body, p) // scale;  p += 4
            mpid   = body[p:p+4].decode('ascii', errors='replace')
            return add_order_mpid(locate=locate, tracking=tracking, timestamp_ns=ts48,
                                  order_ref=oref, side=side, shares=shares,
                                  stock=stock, price=price, mpid=mpid)

        elif mtype == 'E':
            oref   = _read_u64(body, p);     p += 8
            shares = _read_u32(body, p);     p += 4
            match  = _read_u64(body, p)
            return order_executed(locate=locate, tracking=tracking, timestamp_ns=ts48,
                                  order_ref=oref, exec_shares=shares, match_number=match)

        elif mtype == 'C':
            oref   = _read_u64(body, p);     p += 8
            shares = _read_u32(body, p);     p += 4
            match  = _read_u64(body, p);     p += 8
            prtbl  = chr(body[p]) == 'Y';    p += 1
            price  = _read_u32(body, p) // scale
            return order_executed_price(locate=locate, tracking=tracking,
                                        timestamp_ns=ts48, order_ref=oref,
                                        exec_shares=shares, match_number=match,
                                        printable=prtbl, price=price)

        elif mtype == 'X':
            oref   = _read_u64(body, p);     p += 8
            shares = _read_u32(body, p)
            return order_cancel(locate=locate, tracking=tracking, timestamp_ns=ts48,
                                order_ref=oref, cancelled_shares=shares)

        elif mtype == 'D':
            oref = _read_u64(body, p)
            return order_delete(locate=locate, tracking=tracking, timestamp_ns=ts48,
                                order_ref=oref)

        elif mtype == 'U':
            orig   = _read_u64(body, p);     p += 8
            new    = _read_u64(body, p);     p += 8
            shares = _read_u32(body, p);     p += 4
            price  = _read_u32(body, p) // scale
            return order_replace(locate=locate, tracking=tracking, timestamp_ns=ts48,
                                 orig_ref=orig, new_ref=new, shares=shares, price=price)

        elif mtype == 'P':
            oref   = _read_u64(body, p);     p += 8
            side   = chr(body[p]);           p += 1
            shares = _read_u32(body, p);     p += 4
            stock  = _read_stock(body, p);   p += 8
            price  = _read_u32(body, p) // scale;  p += 4
            match  = _read_u64(body, p)
            return trade(locate=locate, tracking=tracking, timestamp_ns=ts48,
                         order_ref=oref, side=side, shares=shares, stock=stock,
                         price=price, match_number=match)

        elif mtype == 'S':
            evt = chr(body[p])
            return system_event(locate=locate, tracking=tracking, timestamp_ns=ts48,
                                event_code=evt)

        else:
            return None  # unsupported type (NOII, RPII, etc.) — silently skip

    except (struct.error, IndexError):
        return None  # truncated / malformed record


# ---- public API -------------------------------------------------------------

def convert_file(src_path: str, dest_path: str,
                 locate_filter: set[int] | None = None,
                 symbol_filter: str | None = None,
                 max_msgs: int = 0,
                 tick_scale: int = 1,
                 after_ns: int = 0,
                 extra_meta: dict | None = None,
                 verbose: bool = True) -> dict:
    """
    Read a real NASDAQ ITCH 5.0 file (optionally .gz), normalise to project
    format, and write to dest_path.

    Args:
        src_path:       Path to real .itch or .itch.gz file.
        dest_path:      Output project-format .itch file.
        locate_filter:  If set, only keep messages with this stock_locate.
        symbol_filter:  If set, only keep Add/Trade messages with this stock name.
                        (Used to build the locate set from the first pass.)
        max_msgs:       Stop after this many output messages (0 = unlimited).
        tick_scale:     Integer-divide all price fields (e.g. 100 = cent grid;
                        makes real symbols fit the RTL's banded FPGA window).
                        Recorded in a <dest>.meta.json sidecar when != 1.
        after_ns:       Skip messages with timestamp < this (ns since midnight);
                        e.g. 34_200e9 = 09:30, avoids pre-market stub quotes
                        mis-centering the auto-set band.
        verbose:        Print progress.

    Returns:
        dict with keys: input_msgs, output_msgs, skipped, bytes_out
    """
    # Open source (plain or gzip)
    if src_path.endswith('.gz'):
        opener = gzip.open
    else:
        opener = open

    out = io.BytesIO()
    stats = {'input_msgs': 0, 'output_msgs': 0, 'skipped': 0, 'bytes_out': 0}
    locates_seen: set[int] = set()

    with opener(src_path, 'rb') as f:
        while True:
            hdr_len_b = f.read(2)
            if len(hdr_len_b) < 2:
                break
            body_len = struct.unpack('>H', hdr_len_b)[0]
            if body_len == 0:
                break
            body = f.read(body_len)
            if len(body) < body_len:
                break  # truncated

            stats['input_msgs'] += 1

            if body_len < _HDR_LEN:
                stats['skipped'] += 1
                continue

            # Parse real ITCH header
            mtype_byte = chr(body[0])
            locate     = _read_u16(body, 1)
            tracking   = _read_u16(body, 3)
            ts48       = _read_ts48(body, 5)

            # Apply locate filter
            if locate_filter is not None and locate not in locate_filter:
                stats['skipped'] += 1
                continue

            if after_ns and ts48 < after_ns:
                stats['skipped'] += 1
                continue

            # Convert to project format
            proj = _convert(body, ts48, locate, tracking, tick_scale)
            if proj is None:
                stats['skipped'] += 1
                continue

            out.write(proj)
            stats['output_msgs'] += 1

            if max_msgs and stats['output_msgs'] >= max_msgs:
                break

    data = out.getvalue()
    stats['bytes_out'] = len(data)
    with open(dest_path, 'wb') as f:
        f.write(data)

    # sidecar so downstream consumers know the price grid was re-scaled
    if tick_scale != 1 or extra_meta:
        import json
        meta = {'source': os.path.basename(src_path),
                'tick_scale': int(tick_scale),
                'output_msgs': stats['output_msgs']}
        if extra_meta:
            meta.update(extra_meta)
        with open(dest_path + '.meta.json', 'w') as f:
            json.dump(meta, f, indent=2)

    if verbose:
        print(f"[nasdaq_itch] {src_path}: "
              f"{stats['input_msgs']:,} in -> {stats['output_msgs']:,} out "
              f"({stats['skipped']:,} skipped) -> {stats['bytes_out']:,} bytes"
              + (f"  [tick_scale={tick_scale}]" if tick_scale != 1 else ""))
    return stats


def find_locates_for_symbol(src_path: str, symbol: str,
                             max_scan: int = 500_000) -> set[int]:
    """
    Scan up to max_scan messages to find the stock_locate values used for
    `symbol` (8-byte padded, as in Add Order messages). Returns a set of
    locate codes; may be empty if the symbol isn't present.
    """
    symbol_padded = (symbol.upper() + '        ')[:8]
    locates: set[int] = set()
    opener = gzip.open if src_path.endswith('.gz') else open

    with opener(src_path, 'rb') as f:
        seen = 0
        while seen < max_scan:
            hdr_b = f.read(2)
            if len(hdr_b) < 2:
                break
            blen = struct.unpack('>H', hdr_b)[0]
            if blen == 0:
                break
            body = f.read(blen)
            if len(body) < blen:
                break
            seen += 1
            if len(body) < 36:
                continue
            if chr(body[0]) not in ('A', 'F', 'P'):
                continue
            # stock field is at offset 11+8+1+4 = 24 in real Add body
            stock = body[24:32].decode('ascii', errors='replace')
            if stock == symbol_padded:
                locates.add(_read_u16(body, 1))

    return locates


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='Convert real NASDAQ ITCH → project format')
    ap.add_argument('src',  help='Input .itch or .itch.gz file')
    ap.add_argument('dest', help='Output project-format .itch file')
    ap.add_argument('--symbol', help='Filter to this ticker (e.g. AAPL)')
    ap.add_argument('--max', type=int, default=0, help='Stop after N output messages')
    ap.add_argument('--tick-scale', type=int, default=1,
                    help='Integer-divide prices (100 = cent grid, fits the FPGA band window)')
    ap.add_argument('--after-hm', default=None, metavar='HH:MM',
                    help='Skip messages before this time of day (e.g. 09:30)')
    args = ap.parse_args()

    after_ns = 0
    if args.after_hm:
        hh, mm = args.after_hm.split(':')
        after_ns = (int(hh) * 3600 + int(mm) * 60) * 1_000_000_000

    loc_filter = None
    if args.symbol:
        print(f'[nasdaq_itch] scanning for locate codes for {args.symbol}...')
        loc_filter = find_locates_for_symbol(args.src, args.symbol)
        if not loc_filter:
            print(f'[nasdaq_itch] WARNING: symbol {args.symbol} not found in first 500k msgs')
        else:
            print(f'[nasdaq_itch] locate codes: {loc_filter}')

    extra = {}
    if args.symbol:
        extra['symbol'] = args.symbol.upper()
        if loc_filter:
            extra['locates'] = sorted(loc_filter)
    convert_file(args.src, args.dest, locate_filter=loc_filter, max_msgs=args.max,
                 tick_scale=args.tick_scale, after_ns=after_ns,
                 extra_meta=extra or None)
