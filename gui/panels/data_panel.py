"""data_panel.py — Data tab: choose a source, build a project .itch, preview it."""
from __future__ import annotations
import os, sys, struct
from pathlib import Path
import streamlit as st
import plotly.graph_objects as go

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))

from gui.panels import theme

_TYPE_NAMES = {
    'A': 'Add', 'F': 'Add (MPID)', 'E': 'Execute', 'C': 'Exec+Price',
    'X': 'Cancel', 'D': 'Delete', 'U': 'Replace', 'P': 'Trade', 'S': 'System',
}
_TYPE_COLOR = {
    'A': '#2563eb', 'F': '#3b82f6', 'E': '#8b5cf6', 'C': '#a855f7',
    'X': '#f59e0b', 'D': '#ef4444', 'U': '#ec4899', 'P': '#059669', 'S': '#94a3b8',
}


def render(state: dict) -> None:
    theme.section('Choose a data source',
                  'Everything downstream — the book replay and the three-way '
                  'latency race — runs on the ITCH file you build here.')

    src_labels = {
        'api': '🌐  Live market API',
        'synthetic': '🧪  Synthetic feed',
        'file': '📼  Real NASDAQ ITCH file',
    }
    choice = st.radio('Source', list(src_labels.values()), horizontal=True,
                      label_visibility='collapsed')
    source = [k for k, v in src_labels.items() if v == choice][0]

    out_dir = str(ROOT / 'gui' / 'data_cache')
    os.makedirs(out_dir, exist_ok=True)

    st.write('')
    if source == 'api':
        _api_form(state, out_dir)
    elif source == 'synthetic':
        _synthetic_form(state, out_dir)
    else:
        _file_form(state, out_dir)

    if state.get('itch_file') and os.path.exists(state['itch_file']):
        st.divider()
        _preview(state['itch_file'])


# ---- source forms -----------------------------------------------------------
def _api_form(state: dict, out_dir: str) -> None:
    theme.note('Pulls recent intraday bars from <b>yfinance</b> (keyless) and '
               'rebuilds a 10-level order book around each bar. The book is a '
               '<b>reconstruction</b> from OHLCV — realistic in shape, not real depth.')
    c1, c2, c3 = st.columns(3)
    ticker   = c1.text_input('Ticker', value='AAPL').upper().strip()
    period   = c2.selectbox('Period', ['1d', '5d', '1mo'], index=1)
    interval = c3.selectbox('Interval', ['1m', '5m', '15m'], index=0)
    with st.expander('Optional API keys (Alpaca / Finnhub) — blank uses yfinance'):
        alpaca_key    = st.text_input('Alpaca API key',  type='password')
        alpaca_secret = st.text_input('Alpaca secret',   type='password')
        finnhub_key   = st.text_input('Finnhub API key', type='password')

    if st.button(f'Pull {ticker or "…"} live', type='primary', disabled=not ticker):
        with st.spinner(f'Fetching {ticker} and rebuilding the book…'):
            from gui.data import make_itch
            try:
                p = make_itch('api', {
                    'ticker': ticker, 'period': period, 'interval': interval,
                    'alpaca_key': alpaca_key, 'alpaca_secret': alpaca_secret,
                    'finnhub_key': finnhub_key,
                }, out_dir=out_dir)
                _loaded(state, p)
            except Exception as e:
                st.error(f'API pull failed: {e}')


def _synthetic_form(state: dict, out_dir: str) -> None:
    theme.note('A deterministic generator emits a mixed Add / Execute / Cancel / '
               'Delete / Replace stream across several symbols — the cleanest way '
               'to stress the engines.')
    c1, c2 = st.columns(2)
    n_msgs    = c1.number_input('Messages', 1_000, 2_000_000, 100_000, 10_000)
    n_symbols = c2.number_input('Symbols', 1, 8, 4)
    if st.button('Generate synthetic feed', type='primary'):
        with st.spinner('Generating…'):
            from gui.data import make_itch
            try:
                p = make_itch('synthetic',
                              {'n_msgs': int(n_msgs), 'n_symbols': int(n_symbols)},
                              out_dir=out_dir)
                _loaded(state, p)
            except Exception as e:
                st.error(f'Generation failed: {e}')


def _file_form(state: dict, out_dir: str) -> None:
    theme.note('Upload a NASDAQ TotalView-ITCH 5.0 file. Real ITCH uses 6-byte '
               'timestamps and the full message set; this normalises it to the '
               'project\'s 8-byte subset. Files are often &gt;1 GB — cap the message count.')
    up = st.file_uploader('ITCH file (.itch / .itch.gz)', type=['itch', 'gz'])
    c1, c2 = st.columns(2)
    symbol   = c1.text_input('Filter symbol (optional)', placeholder='AAPL')
    max_msgs = c2.number_input('Max messages (0 = all)', 0, 5_000_000, 200_000, 50_000)
    if up and st.button('Convert ITCH file', type='primary'):
        raw = os.path.join(out_dir, up.name)
        with open(raw, 'wb') as f:
            f.write(up.read())
        with st.spinner('Converting…'):
            from gui.data import make_itch
            try:
                p = make_itch('file', {'path': raw, 'symbol': symbol.strip() or None,
                                       'max_msgs': int(max_msgs)}, out_dir=out_dir)
                _loaded(state, p)
            except Exception as e:
                st.error(f'Conversion failed: {e}')


def _loaded(state: dict, path: str) -> None:
    state['itch_file'] = path
    state['run_dir'] = ''            # invalidate any prior run
    st.session_state['run_dir'] = ''
    st.success(f'Ready: {os.path.getsize(path):,} bytes. '
               'Head to **Order Book** or **Performance** next.', icon='✅')


# ---- preview ----------------------------------------------------------------
def _scan(path: str):
    counts: dict[str, int] = {}
    total = 0
    with open(path, 'rb') as f:
        while True:
            h = f.read(2)
            if len(h) < 2:
                break
            blen = struct.unpack('>H', h)[0]
            if blen == 0:
                break
            body = f.read(blen)
            if len(body) < blen or not body:
                break
            t = chr(body[0])
            counts[t] = counts.get(t, 0) + 1
            total += 1
    return counts, total


def _preview(path: str) -> None:
    theme.section('Feed preview', 'What the engines will actually consume.')
    meta = theme.load_meta(path)

    try:
        counts, total = _scan(path)
    except Exception as e:
        st.warning(f'Preview parse error: {e}')
        return
    if not counts:
        st.warning('No messages found in file.')
        return

    order = sorted(counts, key=lambda k: counts[k], reverse=True)
    adds   = counts.get('A', 0) + counts.get('F', 0)
    dels   = counts.get('D', 0) + counts.get('X', 0)
    execs  = counts.get('E', 0) + counts.get('C', 0)
    trades = counts.get('P', 0)

    k = st.columns(4)
    theme.kpi(k[0], 'Total messages', theme.fmt_int(total),
              meta.get('label', ''), theme.ACCENT)
    theme.kpi(k[1], 'Adds', theme.fmt_int(adds),
              f'{adds/total*100:.0f}% of stream', '#2563eb')
    theme.kpi(k[2], 'Cancels / Deletes', theme.fmt_int(dels),
              f'{dels/total*100:.0f}% of stream', '#ef4444')
    theme.kpi(k[3], 'Trades', theme.fmt_int(trades),
              f'{execs:,} executions', '#059669')

    st.write('')
    col1, col2 = st.columns([5, 4])

    with col1:
        labels = [_TYPE_NAMES.get(t, t) for t in order]
        vals   = [counts[t] for t in order]
        colors = [_TYPE_COLOR.get(t, '#94a3b8') for t in order]
        bar = go.Figure(go.Bar(
            x=vals, y=labels, orientation='h',
            marker=dict(color=colors),
            text=[theme.fmt_int(v) for v in vals], textposition='auto',
            hovertemplate='%{y}: %{x:,}<extra></extra>'))
        bar.update_layout(title='Message mix', height=340,
                          yaxis=dict(autorange='reversed'),
                          xaxis_title='count')
        theme.show(bar)

    with col2:
        don = go.Figure(go.Pie(
            labels=[_TYPE_NAMES.get(t, t) for t in order],
            values=[counts[t] for t in order],
            hole=.62, sort=False,
            marker=dict(colors=[_TYPE_COLOR.get(t, '#94a3b8') for t in order],
                        line=dict(color='white', width=2)),
            textinfo='percent', textfont=dict(size=11),
            hovertemplate='%{label}: %{value:,} (%{percent})<extra></extra>'))
        don.update_layout(title='Composition', height=340,
                          showlegend=True,
                          legend=dict(orientation='v', x=1.0, y=0.5),
                          annotations=[dict(text=f'{total:,}<br>msgs',
                                            x=.5, y=.5, showarrow=False,
                                            font=dict(size=15, color=theme.INK))])
        theme.show(don)

    if meta.get('source') == 'api' and meta.get('price_ref'):
        st.caption(f"Reconstructed from **{meta.get('bars','?')}** "
                   f"{meta.get('data_source','yfinance')} bars · "
                   f"reference price ${float(meta['price_ref']):.2f} "
                   f"(1 price unit = $0.01).")
