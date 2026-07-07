"""book_panel.py — Order Book tab: DOM ladder, market-depth curve, price & spread."""
from __future__ import annotations
import os, sys
from pathlib import Path
import streamlit as st
import pandas as pd
import plotly.graph_objects as go

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))

from gui.panels import theme


def render(state: dict) -> None:
    run_dir = state.get('run_dir', '')
    if not run_dir or not os.path.isdir(run_dir):
        theme.section('Order book replay')
        st.info('No results yet. Load a feed on **Data**, then open the '
                '**Performance** tab and click **Run pipeline** — that produces '
                'the book snapshots shown here.', icon='📊')
        return

    ladder_path = os.path.join(run_dir, 'ladder.csv')
    depth_path  = os.path.join(run_dir, 'book_depth.csv')
    price_ref = theme.load_meta(state.get('itch_file', '')).get('price_ref')

    if os.path.exists(depth_path):
        _headline(depth_path, price_ref)

    st.divider()
    if os.path.exists(ladder_path):
        _ladder_and_depth(ladder_path, price_ref)
    else:
        st.info('No ladder.csv — re-run the pipeline (the SIMD engine writes it).')

    if os.path.exists(depth_path):
        st.divider()
        _timeseries(depth_path, price_ref)


# ---- headline KPIs ----------------------------------------------------------
def _headline(depth_path: str, price_ref) -> None:
    df = pd.read_csv(depth_path)
    df = df[(df['best_bid'] > 0) & (df['best_ask'] > 0)]
    if df.empty:
        return
    last = df.iloc[-1]
    bid = theme.units_to_dollars(last['best_bid'], price_ref)
    ask = theme.units_to_dollars(last['best_ask'], price_ref)
    mid = (bid + ask) / 2
    spread_c = (ask - bid) * 100
    bd, ad = float(last['bid_depth']), float(last['ask_depth'])
    imb = (bd - ad) / (bd + ad) * 100 if (bd + ad) else 0

    theme.section('Order book replay',
                  'Reconstructed book state as the feed is applied, message by message.')
    k = st.columns(5)
    theme.kpi(k[0], 'Mid price', f'${mid:,.2f}', 'last snapshot', theme.ACCENT)
    theme.kpi(k[1], 'Best bid', f'${bid:,.2f}', theme.fmt_int(bd) + ' sh', theme.BID)
    theme.kpi(k[2], 'Best ask', f'${ask:,.2f}', theme.fmt_int(ad) + ' sh', theme.ASK)
    theme.kpi(k[3], 'Spread', f'{spread_c:,.1f}¢',
              f'{(ask-bid)/mid*1e4:.1f} bps', '#f59e0b')
    lean = 'bid-heavy' if imb > 0 else 'ask-heavy'
    theme.kpi(k[4], 'Imbalance', f'{imb:+.0f}%', lean,
              theme.BID if imb > 0 else theme.ASK)


# ---- ladder + market depth --------------------------------------------------
def _ladder_and_depth(path: str, price_ref) -> None:
    df = pd.read_csv(path)
    if df.empty:
        st.warning('ladder.csv is empty.')
        return
    seqs = sorted(df['seq'].unique())

    theme.section('Depth of market', 'Drag to replay the book at any point in the feed.')
    idx = st.select_slider('Snapshot', options=list(range(len(seqs))),
                           value=len(seqs) // 2,
                           format_func=lambda i: f'msg {seqs[i]:,}',
                           label_visibility='collapsed')
    seq = seqs[idx]
    snap = df[df['seq'] == seq].copy()
    snap['px'] = snap['price'].apply(lambda u: theme.units_to_dollars(u, price_ref))
    bids = snap[snap['side'] == 'bid'].sort_values('price', ascending=False)
    asks = snap[snap['side'] == 'ask'].sort_values('price')

    col1, col2 = st.columns(2)
    with col1:
        _dom(bids, asks)
    with col2:
        _depth_curve(bids, asks)


def _dom(bids: pd.DataFrame, asks: pd.DataFrame) -> None:
    fig = go.Figure()
    if not asks.empty:
        fig.add_trace(go.Bar(
            y=[f'${p:,.2f}' for p in asks['px']], x=asks['shares'],
            orientation='h', name='Asks',
            marker=dict(color=theme.ASK, line=dict(width=0)),
            hovertemplate='ask $%{y} · %{x:,} sh<extra></extra>'))
    if not bids.empty:
        fig.add_trace(go.Bar(
            y=[f'${p:,.2f}' for p in bids['px']], x=bids['shares'],
            orientation='h', name='Bids',
            marker=dict(color=theme.BID, line=dict(width=0)),
            hovertemplate='bid $%{y} · %{x:,} sh<extra></extra>'))
    # price high -> low top to bottom: asks first (desc) then bids (desc)
    order = ([f'${p:,.2f}' for p in asks['px']][::-1] +
             [f'${p:,.2f}' for p in bids['px']])
    fig.update_layout(
        title='Order book ladder', height=430, barmode='overlay',
        yaxis=dict(categoryorder='array', categoryarray=order, title='price'),
        xaxis_title='resting shares',
        legend=dict(x=0.98, xanchor='right', y=1.05))
    theme.show(fig)


def _depth_curve(bids: pd.DataFrame, asks: pd.DataFrame) -> None:
    fig = go.Figure()
    if not bids.empty:
        b = bids.sort_values('px', ascending=False)
        fig.add_trace(go.Scatter(
            x=b['px'], y=b['shares'].cumsum(), name='Bid depth',
            mode='lines', line=dict(color=theme.BID, width=2, shape='hv'),
            fill='tozeroy', fillcolor='rgba(5,150,105,.14)',
            hovertemplate='$%{x:,.2f} · %{y:,} sh cum.<extra></extra>'))
    if not asks.empty:
        a = asks.sort_values('px')
        fig.add_trace(go.Scatter(
            x=a['px'], y=a['shares'].cumsum(), name='Ask depth',
            mode='lines', line=dict(color=theme.ASK, width=2, shape='hv'),
            fill='tozeroy', fillcolor='rgba(239,68,68,.14)',
            hovertemplate='$%{x:,.2f} · %{y:,} sh cum.<extra></extra>'))
    fig.update_layout(title='Cumulative market depth', height=430,
                      xaxis_title='price ($)', yaxis_title='cumulative shares',
                      legend=dict(x=0.5, xanchor='center', y=1.08))
    theme.show(fig)


# ---- time series ------------------------------------------------------------
def _timeseries(path: str, price_ref) -> None:
    df = pd.read_csv(path)
    df = df[(df['best_bid'] > 0) & (df['best_ask'] > 0)].copy()
    if df.empty:
        return
    df['bid'] = df['best_bid'].apply(lambda u: theme.units_to_dollars(u, price_ref))
    df['ask'] = df['best_ask'].apply(lambda u: theme.units_to_dollars(u, price_ref))
    df['mid'] = (df['bid'] + df['ask']) / 2
    df['spread_c'] = (df['ask'] - df['bid']) * 100

    theme.section('Top of book over time', 'Best bid / ask and the spread as the feed streams.')

    fig = go.Figure()
    fig.add_trace(go.Scatter(x=df['seq'], y=df['ask'], name='Best ask',
                             mode='lines', line=dict(color=theme.ASK, width=1),
                             fill=None))
    fig.add_trace(go.Scatter(x=df['seq'], y=df['bid'], name='Best bid',
                             mode='lines', line=dict(color=theme.BID, width=1),
                             fill='tonexty', fillcolor='rgba(99,102,241,.10)'))
    fig.add_trace(go.Scatter(x=df['seq'], y=df['mid'], name='Mid',
                             mode='lines', line=dict(color=theme.ACCENT, width=2, dash='dot')))
    fig.update_layout(title='Best bid / ask (shaded = spread)', height=340,
                      xaxis_title='message sequence', yaxis_title='price ($)')
    theme.show(fig)

    fig2 = go.Figure(go.Scatter(
        x=df['seq'], y=df['spread_c'], name='Spread',
        mode='lines', line=dict(color='#f59e0b', width=1.5),
        fill='tozeroy', fillcolor='rgba(245,158,11,.15)',
        hovertemplate='msg %{x:,} · %{y:.1f}¢<extra></extra>'))
    fig2.update_layout(title='Spread (cents)', height=240,
                       xaxis_title='message sequence', yaxis_title='¢')
    theme.show(fig2)
