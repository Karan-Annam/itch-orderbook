"""Pull live/recent market data and synthesize ITCH messages.

Produces a reconstructed (top-of-book approximation) project-format .itch file
from real trade and quote data.  The book is approximated: each quote update
replaces the outstanding bid/ask with a single order at the new price.  Trades
generate ITCH Trade ('P') messages.  This is labeled as "reconstructed" in the
GUI — it is NOT full order-by-order depth.

Data sources (tried in order):
  1. yfinance — keyless, 1-minute OHLCV + bid/ask for recent intraday data.
  2. Alpaca   — free API key required (set via ALPACA_KEY / ALPACA_SECRET env vars
                or passed as params).
  3. Finnhub  — free API key required (FINNHUB_KEY env var or param).

A session ends with a System Event 'C' so the order book is cleanly closed.
"""
from __future__ import annotations
import io
import os
import sys
import time
import struct
import datetime
from typing import Any

from .itch_writer import (
    system_event, add_order, order_delete, trade as itch_trade,
    dollars_to_units,
)


# ---- internal state for reconstructed-book synthesis -----------------------

class _BookSynthesizer:
    """Generates ITCH bytes that reconstruct a top-of-book from quote/trade data."""

    def __init__(self, locate: int, stock: str):
        self._locate  = locate
        self._stock   = (stock.upper() + '        ')[:8]
        self._next_ref = 1
        self._tracking = 0
        self._bid_ref: int | None = None
        self._ask_ref: int | None = None
        # multi-level ladder state (refs currently resting on each side)
        self._bid_refs: list[int] = []
        self._ask_refs: list[int] = []
        self.ref_close: float | None = None
        self._buf = io.BytesIO()

    def set_ladder(self, dt: datetime.datetime,
                   bids: list[tuple[int, int]],
                   asks: list[tuple[int, int]]) -> None:
        """Replace the whole resting book with new bid/ask ladders.

        `bids`/`asks` are lists of (price_units, shares), best level first.

        Ordering matters for snapshots: we ADD the entire new ladder first,
        then DELETE the old one. That way a snapshot taken at any point during
        the rebuild always sees a fully populated book on both sides (never the
        momentary empty side you get from delete-then-add).
        """
        ts = self._ts(dt)
        loc, stock = self._locate, self._stock

        new_bid_refs: list[int] = []
        new_ask_refs: list[int] = []
        for px, sz in bids:
            if px <= 0 or sz <= 0:
                continue
            r = self._ref()
            self._buf.write(add_order(locate=loc, tracking=self._trk(),
                                      timestamp_ns=ts, order_ref=r, side='B',
                                      shares=sz, stock=stock, price=px))
            new_bid_refs.append(r)
        for px, sz in asks:
            if px <= 0 or sz <= 0:
                continue
            r = self._ref()
            self._buf.write(add_order(locate=loc, tracking=self._trk(),
                                      timestamp_ns=ts, order_ref=r, side='S',
                                      shares=sz, stock=stock, price=px))
            new_ask_refs.append(r)
        for r in self._bid_refs + self._ask_refs:
            self._buf.write(order_delete(locate=loc, tracking=self._trk(),
                                         timestamp_ns=ts, order_ref=r))
        self._bid_refs = new_bid_refs
        self._ask_refs = new_ask_refs

    def _ts(self, dt: datetime.datetime) -> int:
        """Convert datetime to ns-since-midnight (project format)."""
        midnight = dt.replace(hour=0, minute=0, second=0, microsecond=0,
                              tzinfo=dt.tzinfo)
        return int((dt - midnight).total_seconds() * 1e9)

    def _ref(self) -> int:
        r = self._next_ref
        self._next_ref += 1
        return r

    def _trk(self) -> int:
        self._tracking += 1
        return self._tracking

    def update_quote(self, dt: datetime.datetime,
                     bid_price: float, bid_size: int,
                     ask_price: float, ask_size: int) -> None:
        """Replace outstanding bid/ask (prices in dollars)."""
        self.update_quote_units(dt, dollars_to_units(bid_price), bid_size,
                                dollars_to_units(ask_price), ask_size)

    def update_quote_units(self, dt: datetime.datetime,
                           bid_units: int, bid_size: int,
                           ask_units: int, ask_size: int) -> None:
        """Replace outstanding bid/ask (prices already in project price units)."""
        ts = self._ts(dt)
        loc, stock = self._locate, self._stock

        # Bid: delete old, add new
        if self._bid_ref is not None:
            self._buf.write(order_delete(
                locate=loc, tracking=self._trk(), timestamp_ns=ts,
                order_ref=self._bid_ref))
        if bid_units > 0 and bid_size > 0:
            r = self._ref()
            self._buf.write(add_order(
                locate=loc, tracking=self._trk(), timestamp_ns=ts,
                order_ref=r, side='B', shares=bid_size, stock=stock,
                price=bid_units))
            self._bid_ref = r
        else:
            self._bid_ref = None

        # Ask: delete old, add new
        if self._ask_ref is not None:
            self._buf.write(order_delete(
                locate=loc, tracking=self._trk(), timestamp_ns=ts,
                order_ref=self._ask_ref))
        if ask_units > 0 and ask_size > 0:
            r = self._ref()
            self._buf.write(add_order(
                locate=loc, tracking=self._trk(), timestamp_ns=ts,
                order_ref=r, side='S', shares=ask_size, stock=stock,
                price=ask_units))
            self._ask_ref = r
        else:
            self._ask_ref = None

    def add_trade(self, dt: datetime.datetime,
                  price: float, shares: int, match: int) -> None:
        """Add a trade (price in dollars)."""
        self.add_trade_units(dt, dollars_to_units(price), shares, match)

    def add_trade_units(self, dt: datetime.datetime,
                        price_units: int, shares: int, match: int) -> None:
        """Add a trade (price already in project price units)."""
        ts = self._ts(dt)
        self._buf.write(itch_trade(
            locate=self._locate, tracking=self._trk(), timestamp_ns=ts,
            order_ref=0, side='B', shares=shares, stock=self._stock,
            price=price_units, match_number=match))

    def close(self, dt: datetime.datetime) -> None:
        ts = self._ts(dt)
        self._buf.write(system_event(
            locate=self._locate, tracking=self._trk(), timestamp_ns=ts,
            event_code='C'))

    def to_bytes(self) -> bytes:
        return self._buf.getvalue()


# ---- yfinance data source ---------------------------------------------------

def _fetch_yfinance(ticker: str, period: str = '1d',
                    interval: str = '1m') -> dict | None:
    """
    Fetch OHLCV + fast-info bid/ask from yfinance.
    Returns dict with keys: rows (list of dicts with ts/open/high/low/close/volume),
    bid, ask, bid_size, ask_size, or None on failure.
    """
    try:
        import yfinance as yf  # type: ignore
    except ImportError:
        return None

    try:
        tk = yf.Ticker(ticker)
        hist = tk.history(period=period, interval=interval, auto_adjust=True)
        if hist.empty:
            return None

        rows = []
        for ts_idx, row in hist.iterrows():
            rows.append({
                'ts': ts_idx.to_pydatetime(),
                'open':   float(row['Open']),
                'high':   float(row['High']),
                'low':    float(row['Low']),
                'close':  float(row['Close']),
                'volume': int(row.get('Volume', 0)),
            })

        # Current bid/ask from fast_info (may be None outside hours)
        try:
            info = tk.fast_info
            bid      = float(getattr(info, 'bid',       0) or 0)
            ask      = float(getattr(info, 'ask',       0) or 0)
            bid_size = int(  getattr(info, 'bid_size',  100) or 100)
            ask_size = int(  getattr(info, 'ask_size',  100) or 100)
        except Exception:
            last = rows[-1]['close'] if rows else 0
            bid, ask = last * 0.999, last * 1.001
            bid_size = ask_size = 100

        return {'rows': rows, 'bid': bid, 'ask': ask,
                'bid_size': bid_size, 'ask_size': ask_size}
    except Exception as e:
        print(f'[api_adapter] yfinance error for {ticker}: {e}')
        return None


def _synthesize_from_ohlcv(rows: list[dict], synth: _BookSynthesizer,
                            tick_frac: float = 0.0006, levels: int = 10) -> None:
    """
    Convert OHLCV bars into a realistic multi-level ITCH order book.

    Each bar rebuilds a `levels`-deep ladder on each side around the bar's
    close, then prints one Trade near the mid. Prices are normalised into the
    project's price space so high-priced names (AAPL ~$280) fit inside the
    book's 1M-level capacity:

        mid_units = 500_000 + round((close - ref_close) * 100)   # 1 unit = $0.01

    Bids sit at mid-tick, mid-2·tick, …; asks at mid+tick, mid+2·tick, ….
    Sizes fan out with depth (thin at the touch, thicker deeper) plus mild,
    deterministic jitter so the ladder looks like a real DOM rather than a
    staircase. Everything is clamped to [100_001, 899_999] so the book is never
    crossed and never runs off the end of price space.

    ref_close (first valid close) is stored on `synth` so the GUI can reverse
    the mapping: dollar_price = ref + (units - 500_000) / 100.
    """
    import random
    rng = random.Random(0xB00C)
    match_num = 1
    ref_close: float | None = None

    for row in rows:
        dt     = row['ts']
        close  = row['close']
        volume = row['volume']
        if close <= 0:
            continue
        if ref_close is None:
            ref_close = close
            synth.ref_close = close

        tick_units = max(1, int(round(max(0.01, close * tick_frac) * 100)))
        mid_units  = 500_000 + int(round((close - ref_close) * 100))

        # size scale from bar volume, spread across the ladder
        base = max(100, int(volume // (levels * 4)) if volume else 300)

        bids: list[tuple[int, int]] = []
        asks: list[tuple[int, int]] = []
        for i in range(1, levels + 1):
            wobble = 0.75 + 0.5 * rng.random()
            depthw = 0.6 + 0.28 * i                    # thicker deeper in book
            sz = max(100, int(base * depthw * wobble))
            bpx = mid_units - i * tick_units
            apx = mid_units + i * tick_units
            if 100_001 <= bpx <= 499_999:
                bids.append((bpx, sz))
            if 500_001 <= apx <= 899_999:
                asks.append((apx, sz))

        if not bids and not asks:
            continue
        synth.set_ladder(dt, bids, asks)

        if volume > 0:
            trade_units = mid_units
            trade_units = max(100_002, min(899_998, trade_units))
            synth.add_trade_units(dt, trade_units, min(volume, 99_999), match_num)
            match_num += 1


def _fetch_alpaca(ticker: str, api_key: str, api_secret: str,
                  start: str, end: str) -> list[dict] | None:
    """
    Fetch 1-minute bars from Alpaca (requires alpaca-py or requests).
    Returns list of OHLCV row dicts, or None on failure.
    """
    try:
        import requests  # type: ignore
        url = f'https://data.alpaca.markets/v2/stocks/{ticker}/bars'
        headers = {'APCA-API-KEY-ID': api_key, 'APCA-API-SECRET-KEY': api_secret}
        params  = {'timeframe': '1Min', 'start': start, 'end': end, 'limit': 1000}
        r = requests.get(url, headers=headers, params=params, timeout=10)
        r.raise_for_status()
        bars = r.json().get('bars', [])
        rows = []
        for b in bars:
            dt = datetime.datetime.fromisoformat(b['t'].replace('Z', '+00:00'))
            rows.append({'ts': dt, 'open': b['o'], 'high': b['h'],
                         'low': b['l'], 'close': b['c'], 'volume': b['v']})
        return rows or None
    except Exception as e:
        print(f'[api_adapter] Alpaca error: {e}')
        return None


def _fetch_finnhub(ticker: str, api_key: str, resolution: str = '1') -> list[dict] | None:
    """
    Fetch recent 1-minute bars from Finnhub.
    Returns list of OHLCV row dicts, or None on failure.
    """
    try:
        import requests  # type: ignore
        now   = int(time.time())
        start = now - 7 * 24 * 3600
        url   = 'https://finnhub.io/api/v1/stock/candle'
        params = {'symbol': ticker, 'resolution': resolution,
                  'from': start, 'to': now, 'token': api_key}
        r = requests.get(url, params=params, timeout=10)
        r.raise_for_status()
        data = r.json()
        if data.get('s') != 'ok':
            return None
        rows = []
        for ts_unix, o, h, l, c, v in zip(data['t'], data['o'], data['h'],
                                            data['l'], data['c'], data['v']):
            dt = datetime.datetime.fromtimestamp(ts_unix,
                                                 tz=datetime.timezone.utc)
            rows.append({'ts': dt, 'open': o, 'high': h,
                         'low': l, 'close': c, 'volume': int(v)})
        return rows or None
    except Exception as e:
        print(f'[api_adapter] Finnhub error: {e}')
        return None


# ---- public API -------------------------------------------------------------

def fetch_to_itch(ticker: str, dest_path: str,
                  alpaca_key: str = '', alpaca_secret: str = '',
                  finnhub_key: str = '',
                  period: str = '1d', interval: str = '1m',
                  locate: int = 1, verbose: bool = True) -> dict:
    """
    Pull market data for `ticker` and write a reconstructed project-format .itch.

    Tries: yfinance (keyless) → Alpaca (if keys set) → Finnhub (if key set).
    Raises RuntimeError if all sources fail.

    Returns dict: {'rows': N, 'trades': N, 'bytes': N, 'source': str}
    """
    rows: list[dict] | None = None
    source = 'none'

    # Try yfinance first (keyless, always available)
    data = _fetch_yfinance(ticker, period=period, interval=interval)
    if data and data['rows']:
        rows = data['rows']
        source = 'yfinance'

    # Fall back to Alpaca
    if rows is None:
        akey = alpaca_key or os.environ.get('ALPACA_KEY', '')
        asec = alpaca_secret or os.environ.get('ALPACA_SECRET', '')
        if akey and asec:
            today = datetime.date.today().isoformat()
            yesterday = (datetime.date.today() - datetime.timedelta(days=1)).isoformat()
            rows = _fetch_alpaca(ticker, akey, asec, yesterday, today)
            if rows:
                source = 'alpaca'

    # Fall back to Finnhub
    if rows is None:
        fkey = finnhub_key or os.environ.get('FINNHUB_KEY', '')
        if fkey:
            rows = _fetch_finnhub(ticker, fkey)
            if rows:
                source = 'finnhub'

    if not rows:
        raise RuntimeError(
            f'All data sources failed for {ticker}. '
            'Install yfinance (pip install yfinance) or provide API keys.')

    synth = _BookSynthesizer(locate=locate, stock=ticker)
    # System Start
    dt0 = rows[0]['ts']
    ts0 = synth._ts(dt0)
    synth._buf.write(system_event(locate=locate, tracking=0,
                                  timestamp_ns=ts0, event_code='O'))
    _synthesize_from_ohlcv(rows, synth)
    synth.close(rows[-1]['ts'])

    data_bytes = synth.to_bytes()
    with open(dest_path, 'wb') as f:
        f.write(data_bytes)

    result = {'rows': len(rows), 'bytes': len(data_bytes), 'source': source,
              'ticker': ticker, 'price_ref': getattr(synth, 'ref_close', None),
              'levels': 10}
    if verbose:
        print(f'[api_adapter] {ticker}: {len(rows)} bars from {source} -> '
              f'{len(data_bytes):,} bytes -> {dest_path}')
    return result


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description='Pull market data → project-format ITCH')
    ap.add_argument('ticker', help='Ticker symbol, e.g. AAPL')
    ap.add_argument('dest',   help='Output .itch file path')
    ap.add_argument('--period',   default='1d')
    ap.add_argument('--interval', default='1m')
    ap.add_argument('--locate',   type=int, default=1)
    args = ap.parse_args()

    result = fetch_to_itch(args.ticker, args.dest,
                           period=args.period, interval=args.interval,
                           locate=args.locate)
    print(result)
