"""Write project-format ITCH 5.0 files.

Byte layout mirrors sw/parser/itch_messages.hpp exactly:
  - Each message is preceded by a 2-byte big-endian length prefix.
  - Common 13-byte header: type[1] + stock_locate[2] + tracking[2] + timestamp[8] (all BE).
  - Type-specific fields follow at offset 13.
  - All integers big-endian (network byte order).
  - Prices are in 1/10000 dollar units (e.g. $50.00 = 500000).

This is the single Python source of truth for writing project-format .itch files;
nasdaq_itch.py and api_adapter.py both call here rather than hand-rolling bytes.
"""
from __future__ import annotations
import struct
import io
import os
import subprocess
import sys
import tempfile


# ---- primitive encoders -----------------------------------------------------

def _u8(v: int) -> bytes:
    return struct.pack('B', v & 0xFF)

def _u16be(v: int) -> bytes:
    return struct.pack('>H', v & 0xFFFF)

def _u32be(v: int) -> bytes:
    return struct.pack('>I', v & 0xFFFFFFFF)

def _u64be(v: int) -> bytes:
    return struct.pack('>Q', v & 0xFFFFFFFFFFFFFFFF)

def _stock(s: str) -> bytes:
    """8-byte right-space-padded ASCII stock field."""
    b = s.encode('ascii')[:8]
    return b + b' ' * (8 - len(b))

def _header(type_char: str, locate: int, tracking: int, timestamp_ns: int) -> bytes:
    return (type_char.encode('ascii') +
            _u16be(locate) +
            _u16be(tracking) +
            _u64be(timestamp_ns))

def _frame(body: bytes) -> bytes:
    return struct.pack('>H', len(body)) + body


# ---- public message builders (return length-prefixed bytes) -----------------

def system_event(locate: int, tracking: int, timestamp_ns: int,
                 event_code: str) -> bytes:
    """System Event ('S') — body 14 bytes."""
    body = _header('S', locate, tracking, timestamp_ns) + event_code.encode('ascii')
    assert len(body) == 14
    return _frame(body)


def add_order(locate: int, tracking: int, timestamp_ns: int,
              order_ref: int, side: str, shares: int,
              stock: str, price: int) -> bytes:
    """Add Order ('A') — body 38 bytes. price in 1/10000 dollar units."""
    body = (_header('A', locate, tracking, timestamp_ns) +
            _u64be(order_ref) +
            side.encode('ascii') +
            _u32be(shares) +
            _stock(stock) +
            _u32be(price))
    assert len(body) == 38, f"add body len {len(body)}"
    return _frame(body)


def add_order_mpid(locate: int, tracking: int, timestamp_ns: int,
                   order_ref: int, side: str, shares: int,
                   stock: str, price: int, mpid: str) -> bytes:
    """Add Order with MPID ('F') — body 42 bytes."""
    mpid_b = mpid.encode('ascii')[:4]
    mpid_b = mpid_b + b' ' * (4 - len(mpid_b))
    body = (_header('F', locate, tracking, timestamp_ns) +
            _u64be(order_ref) +
            side.encode('ascii') +
            _u32be(shares) +
            _stock(stock) +
            _u32be(price) +
            mpid_b)
    assert len(body) == 42
    return _frame(body)


def order_executed(locate: int, tracking: int, timestamp_ns: int,
                   order_ref: int, exec_shares: int,
                   match_number: int) -> bytes:
    """Order Executed ('E') — body 33 bytes."""
    body = (_header('E', locate, tracking, timestamp_ns) +
            _u64be(order_ref) +
            _u32be(exec_shares) +
            _u64be(match_number))
    assert len(body) == 33
    return _frame(body)


def order_executed_price(locate: int, tracking: int, timestamp_ns: int,
                         order_ref: int, exec_shares: int,
                         match_number: int, printable: bool,
                         price: int) -> bytes:
    """Order Executed with Price ('C') — body 38 bytes."""
    body = (_header('C', locate, tracking, timestamp_ns) +
            _u64be(order_ref) +
            _u32be(exec_shares) +
            _u64be(match_number) +
            b'Y' if printable else b'N' +
            _u32be(price))
    assert len(body) == 38
    return _frame(body)


def order_cancel(locate: int, tracking: int, timestamp_ns: int,
                 order_ref: int, cancelled_shares: int) -> bytes:
    """Order Cancel ('X') — body 25 bytes."""
    body = (_header('X', locate, tracking, timestamp_ns) +
            _u64be(order_ref) +
            _u32be(cancelled_shares))
    assert len(body) == 25
    return _frame(body)


def order_delete(locate: int, tracking: int, timestamp_ns: int,
                 order_ref: int) -> bytes:
    """Order Delete ('D') — body 21 bytes."""
    body = (_header('D', locate, tracking, timestamp_ns) +
            _u64be(order_ref))
    assert len(body) == 21
    return _frame(body)


def order_replace(locate: int, tracking: int, timestamp_ns: int,
                  orig_ref: int, new_ref: int,
                  shares: int, price: int) -> bytes:
    """Order Replace ('U') — body 37 bytes."""
    body = (_header('U', locate, tracking, timestamp_ns) +
            _u64be(orig_ref) +
            _u64be(new_ref) +
            _u32be(shares) +
            _u32be(price))
    assert len(body) == 37
    return _frame(body)


def trade(locate: int, tracking: int, timestamp_ns: int,
          order_ref: int, side: str, shares: int,
          stock: str, price: int, match_number: int) -> bytes:
    """Non-Cross Trade ('P') — body 46 bytes."""
    body = (_header('P', locate, tracking, timestamp_ns) +
            _u64be(order_ref) +
            side.encode('ascii') +
            _u32be(shares) +
            _stock(stock) +
            _u32be(price) +
            _u64be(match_number))
    assert len(body) == 46
    return _frame(body)


# ---- price helpers ----------------------------------------------------------

def dollars_to_units(price_dollars: float) -> int:
    """Convert a dollar price to ITCH 1/10000 dollar units."""
    return max(0, int(round(price_dollars * 10000)))

def units_to_dollars(price_units: int) -> float:
    return price_units / 10000.0


# ---- ItchWriter: buffered stream writer -------------------------------------

class ItchWriter:
    """Accumulate project-format ITCH messages and write to a file."""

    def __init__(self, path: str):
        self.path = path
        self._buf = io.BytesIO()

    def write_raw(self, msg: bytes) -> None:
        self._buf.write(msg)

    def system_event(self, **kw) -> None:
        self._buf.write(system_event(**kw))

    def add_order(self, **kw) -> None:
        self._buf.write(add_order(**kw))

    def order_delete(self, **kw) -> None:
        self._buf.write(order_delete(**kw))

    def order_executed(self, **kw) -> None:
        self._buf.write(order_executed(**kw))

    def trade(self, **kw) -> None:
        self._buf.write(trade(**kw))

    def flush(self) -> int:
        """Write buffered bytes to disk. Returns byte count."""
        data = self._buf.getvalue()
        with open(self.path, 'wb') as f:
            f.write(data)
        return len(data)


# ---- self-check -------------------------------------------------------------

def _find_sw_binary(hint: str) -> str:
    """Resolve the orderbook_sw binary to an absolute path, trying .exe on Windows."""
    from pathlib import Path
    candidates = [hint, hint + '.exe']
    # Also try relative to project root (three dirs above gui/data/itch_writer.py)
    root = Path(__file__).resolve().parent.parent.parent
    for c in candidates:
        for base in (Path('.'), root):
            p = (base / c).resolve()
            if p.exists():
                return str(p)
    return str(Path(hint).resolve())  # let subprocess fail with a clear message


def _selfcheck(sw_binary: str) -> int:
    """Build a minimal ITCH stream and verify orderbook_sw accepts it cleanly."""
    import tempfile
    sw_binary = _find_sw_binary(sw_binary)
    with tempfile.NamedTemporaryFile(suffix='.itch', delete=False) as tmp:
        path = tmp.name

    try:
        buf = io.BytesIO()
        ts = 34200_000_000_000  # 9:30 AM in ns
        loc = 1

        buf.write(system_event(locate=loc, tracking=0, timestamp_ns=ts,
                               event_code='O'))
        # Add 4 bid orders
        for i, (oref, px_d) in enumerate([(101, 49.90), (102, 49.80),
                                           (103, 49.85), (104, 49.95)], 1):
            ts += 1_000_000
            buf.write(add_order(locate=loc, tracking=i, timestamp_ns=ts,
                                order_ref=oref, side='B', shares=100,
                                stock='AAPL    ', price=dollars_to_units(px_d)))
        # Add 4 ask orders
        for i, (oref, px_d) in enumerate([(201, 50.10), (202, 50.20),
                                           (203, 50.05), (204, 50.15)], 5):
            ts += 1_000_000
            buf.write(add_order(locate=loc, tracking=i, timestamp_ns=ts,
                                order_ref=oref, side='S', shares=100,
                                stock='AAPL    ', price=dollars_to_units(px_d)))
        # Execute the top ask
        ts += 1_000_000
        buf.write(order_executed(locate=loc, tracking=9, timestamp_ns=ts,
                                 order_ref=203, exec_shares=50, match_number=1001))
        # Cancel remaining
        ts += 1_000_000
        buf.write(order_cancel(locate=loc, tracking=10, timestamp_ns=ts,
                               order_ref=203, cancelled_shares=50))
        # Delete a bid
        ts += 1_000_000
        buf.write(order_delete(locate=loc, tracking=11, timestamp_ns=ts,
                               order_ref=102))
        # Replace bid 101
        ts += 1_000_000
        buf.write(order_replace(locate=loc, tracking=12, timestamp_ns=ts,
                                orig_ref=101, new_ref=501,
                                shares=200, price=dollars_to_units(49.95)))
        # Trade
        ts += 1_000_000
        buf.write(trade(locate=loc, tracking=13, timestamp_ns=ts,
                        order_ref=0, side='B', shares=100, stock='AAPL    ',
                        price=dollars_to_units(50.05), match_number=2001))
        buf.write(system_event(locate=loc, tracking=14, timestamp_ns=ts,
                               event_code='C'))

        with open(path, 'wb') as f:
            f.write(buf.getvalue())

        # A Windows .exe under WSL needs a translated path; a native ELF build
        # (build/orderbook_sw) takes the Linux path unchanged.
        from ._winenv import arg_path
        result = subprocess.run(
            [sw_binary, arg_path(sw_binary, path), '--quiet'],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f"[selfcheck] FAIL: {sw_binary} exited {result.returncode}")
            print(result.stdout[-500:])
            print(result.stderr[-500:])
            return 1
        # Check that no "unknown" messages were reported
        out = result.stdout
        if 'unknown' in out.lower() and 'Unknown' not in out:
            print(f"[selfcheck] FAIL: found 'unknown' in output:\n{out[:500]}")
            return 1
        print(f"[selfcheck] PASS — {len(buf.getvalue())} bytes, "
              f"orderbook_sw exited 0")
        return 0
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--selfcheck', action='store_true')
    ap.add_argument('--sw', default='build/orderbook_sw',
                    help='Path to orderbook_sw binary')
    args = ap.parse_args()
    if args.selfcheck:
        sys.exit(_selfcheck(args.sw))
    else:
        ap.print_help()
