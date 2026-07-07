"""gui/data — project-format ITCH file production layer.

Public entry point: make_itch(source, params) -> path

source values:
  'synthetic'  generate via build/gen_itch
  'file'       normalise a real NASDAQ ITCH 5.0 file
  'api'        pull from live market API (yfinance / Alpaca / Finnhub)
"""
from __future__ import annotations
import json
import os
import subprocess
import tempfile
import datetime
from pathlib import Path


def _write_meta(dest: str, meta: dict) -> None:
    """Write a <dest>.meta.json sidecar the GUI panels read for price mapping."""
    meta = dict(meta)
    meta.setdefault('generated', datetime.datetime.now().isoformat(timespec='seconds'))
    try:
        with open(dest + '.meta.json', 'w') as f:
            json.dump(meta, f, indent=2)
    except OSError:
        pass


def make_itch(source: str, params: dict, out_dir: str = '.') -> str:
    """
    Produce a project-format .itch file and return its path.

    source='synthetic':
        params: n_msgs (int, default 100000), n_symbols (int, default 4)

    source='file':
        params: path (str, required), symbol (str, optional), max_msgs (int, optional)

    source='api':
        params: ticker (str, required), period (str, '1d'), interval (str, '1m'),
                alpaca_key, alpaca_secret, finnhub_key (all optional)
    """
    os.makedirs(out_dir, exist_ok=True)

    if source == 'synthetic':
        n_msgs    = int(params.get('n_msgs', 100_000))
        n_symbols = int(params.get('n_symbols', 4))
        dest = os.path.join(out_dir, f'synthetic_{n_msgs}.itch')
        _gen_synthetic(n_msgs, n_symbols, dest)
        _write_meta(dest, {'source': 'synthetic', 'label': f'Synthetic · {n_symbols} symbols',
                           'price_ref': None, 'symbols': n_symbols})
        return dest

    elif source == 'file':
        src = params['path']
        symbol = params.get('symbol')
        max_msgs = int(params.get('max_msgs', 0))
        name = Path(src).stem + '_proj.itch'
        dest = os.path.join(out_dir, name)
        from .nasdaq_itch import convert_file, find_locates_for_symbol
        loc_filter = None
        if symbol:
            loc_filter = find_locates_for_symbol(src, symbol)
        convert_file(src, dest, locate_filter=loc_filter, max_msgs=max_msgs)
        _write_meta(dest, {'source': 'file', 'label': f'Real ITCH · {Path(src).name}',
                           'price_ref': None, 'symbol': symbol or 'all'})
        return dest

    elif source == 'api':
        ticker   = params['ticker']
        period   = params.get('period',   '1d')
        interval = params.get('interval', '1m')
        dest = os.path.join(out_dir, f'{ticker}_{period}.itch')
        from .api_adapter import fetch_to_itch
        res = fetch_to_itch(ticker, dest,
                            alpaca_key    = params.get('alpaca_key', ''),
                            alpaca_secret = params.get('alpaca_secret', ''),
                            finnhub_key   = params.get('finnhub_key', ''),
                            period=period, interval=interval)
        _write_meta(dest, {'source': 'api', 'label': f'{ticker} · {period}/{interval}',
                           'ticker': ticker, 'period': period, 'interval': interval,
                           'price_ref': res.get('price_ref'),
                           'data_source': res.get('source'), 'bars': res.get('rows')})
        return dest

    else:
        raise ValueError(f'Unknown source: {source!r}')


def _gen_synthetic(n_msgs: int, n_symbols: int, dest: str) -> None:
    """Run build/gen_itch (or the .exe variant) to produce a synthetic file.

    The binaries are native Windows .exe files. Under WSL we launch them via
    interop but must hand them a Windows-form path (C:\\...), so the .exe can
    create the output file. Python still returns the original `dest` (Linux
    path) to its caller for its own reads. On any failure we fall back to the
    pure-Python generator so the GUI always produces a file.
    """
    from ._winenv import arg_path

    root = Path(__file__).parent.parent.parent  # orderbook_project/
    for candidate in ['build/gen_itch', 'build/gen_itch.exe']:
        binary = root / candidate
        if not binary.exists():
            continue
        # gen_itch <dest> <n_msgs> <n_symbols>. Native ELF takes the Linux path;
        # a Windows .exe under WSL needs a translated path.
        try:
            subprocess.run(
                [str(binary), arg_path(binary, dest), str(n_msgs), str(n_symbols)],
                check=True, capture_output=True, text=True)
            if os.path.exists(dest) and os.path.getsize(dest) > 0:
                return
        except (subprocess.CalledProcessError, OSError) as e:
            # gen_itch failed (e.g. path/interop issue) — fall back to Python.
            print(f'[data] gen_itch failed ({e}); using Python generator')
        break

    # Fallback: use itch_writer.py to synthesize a minimal stream in-process.
    _gen_synthetic_python(n_msgs, dest)


def _gen_synthetic_python(n_msgs: int, dest: str) -> None:
    """Minimal Python synthetic generator (no C++ binary required)."""
    import io, random
    from .itch_writer import (
        system_event, add_order, order_delete, order_executed, order_replace,
        dollars_to_units,
    )

    buf = io.BytesIO()
    ts    = 34_200_000_000_000
    oref  = 1
    match = 1
    live: list[tuple[int, str, int]] = []  # (oref, side, price_units)
    MID_UNITS = dollars_to_units(50.00)
    TICK = 100  # 1 cent in 1/10000 units

    buf.write(system_event(locate=1, tracking=0, timestamp_ns=ts, event_code='O'))
    rng = random.Random(42)

    for i in range(n_msgs):
        ts += 1_000_000
        op = rng.random()

        if op < 0.50 or not live:
            # Add
            side = 'B' if rng.random() < 0.5 else 'S'
            if side == 'B':
                px = MID_UNITS - rng.randint(1, 50) * TICK
            else:
                px = MID_UNITS + rng.randint(1, 50) * TICK
            shares = rng.randint(1, 500) * 100
            buf.write(add_order(locate=1, tracking=i, timestamp_ns=ts,
                                order_ref=oref, side=side, shares=shares,
                                stock='SYNTH   ', price=px))
            live.append((oref, side, px))
            oref += 1
        elif op < 0.70 and live:
            # Delete
            idx = rng.randrange(len(live))
            ref, _, _ = live.pop(idx)
            buf.write(order_delete(locate=1, tracking=i, timestamp_ns=ts,
                                   order_ref=ref))
        else:
            # Execute top of book (approximately)
            bids = [(r, p) for r, s, p in live if s == 'B']
            asks = [(r, p) for r, s, p in live if s == 'S']
            if bids and asks:
                best_bid_ref, _ = max(bids, key=lambda x: x[1])
                buf.write(order_executed(locate=1, tracking=i, timestamp_ns=ts,
                                         order_ref=best_bid_ref, exec_shares=100,
                                         match_number=match))
                live = [(r, s, p) for r, s, p in live if r != best_bid_ref]
                match += 1

    buf.write(system_event(locate=1, tracking=n_msgs + 1, timestamp_ns=ts,
                           event_code='C'))

    with open(dest, 'wb') as f:
        f.write(buf.getvalue())
