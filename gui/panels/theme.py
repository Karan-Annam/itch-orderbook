"""theme.py — shared visual language for the Order Book Lab GUI.

One place for the palette, the Plotly template, page CSS, and small render
helpers (KPI cards, section headers, chart wrapper, formatters). Every panel
imports from here so the whole app looks like one product.
"""
from __future__ import annotations
import json
import os
from pathlib import Path

import streamlit as st
import plotly.graph_objects as go
import plotly.io as pio


# ---- palette ----------------------------------------------------------------
INK    = '#0f172a'   # slate-900  primary text
MUTED  = '#64748b'   # slate-500  secondary text
FAINT  = '#94a3b8'   # slate-400
BORDER = '#e2e8f0'   # slate-200
PANEL  = '#f8fafc'   # slate-50
GRID   = 'rgba(100,116,139,0.16)'
ACCENT = '#6366f1'   # indigo-500

# engine identity — used everywhere the three implementations are compared
ENGINE_COLOR = {
    'simd': '#2563eb',   # blue-600
    'map':  '#f59e0b',   # amber-500
    'hw':   '#059669',   # emerald-600
}
ENGINE_LABEL = {
    'simd': 'C++ SIMD',
    'map':  'C++ std::map',
    'hw':   'RTL hardware',
}
ENGINE_LONG = {
    'simd': 'Direct-indexed + SIMD',
    'map':  'std::map reference',
    'hw':   'SystemVerilog @ 250 MHz',
}

BID = '#059669'   # emerald-600
ASK = '#ef4444'   # red-500


# ---- plotly template --------------------------------------------------------
def install_template() -> None:
    axis = dict(
        gridcolor=GRID, zerolinecolor=GRID, linecolor=BORDER,
        tickfont=dict(color=MUTED, size=12),
        title=dict(font=dict(color=MUTED, size=13)),
        showline=False, ticks='outside', tickcolor=BORDER, ticklen=4,
    )
    pio.templates['oblab'] = go.layout.Template(layout=dict(
        font=dict(family='-apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif',
                  size=13, color=INK),
        paper_bgcolor='rgba(0,0,0,0)',
        plot_bgcolor='rgba(0,0,0,0)',
        colorway=['#2563eb', '#f59e0b', '#059669', '#6366f1', '#ec4899', '#14b8a6'],
        xaxis=axis, yaxis=axis,
        margin=dict(t=54, b=44, l=60, r=24),
        hoverlabel=dict(bgcolor='white', bordercolor=BORDER,
                        font=dict(color=INK, size=12)),
        legend=dict(orientation='h', yanchor='bottom', y=1.02, x=0,
                    font=dict(color=MUTED, size=12),
                    bgcolor='rgba(0,0,0,0)'),
        title=dict(x=0, xanchor='left', font=dict(color=INK, size=16)),
        colorscale=dict(sequential=[[0, '#eff6ff'], [1, '#1e3a8a']]),
    ))
    pio.templates.default = 'plotly_white+oblab'


# ---- page CSS ---------------------------------------------------------------
_CSS = """
<style>
  .block-container { padding-top: 2.2rem; padding-bottom: 4rem; max-width: 1360px; }
  #MainMenu, footer { visibility: hidden; }

  /* hero */
  .ob-hero h1 { font-size: 2.0rem; font-weight: 800; letter-spacing:-0.02em;
                margin: 0 0 .15rem 0; color: #0f172a; }
  .ob-hero p  { color:#64748b; font-size: 1.02rem; margin:.1rem 0 .2rem 0; }
  .ob-chips   { display:flex; gap:.4rem; flex-wrap:wrap; margin:.7rem 0 .2rem; }
  .ob-chip    { font-size:.74rem; font-weight:600; padding:.2rem .6rem;
                border-radius:999px; border:1px solid #e2e8f0; background:#f8fafc;
                color:#475569; }
  .ob-chip b  { color:#0f172a; }

  /* section header */
  .ob-sec { margin: .3rem 0 .1rem; }
  .ob-sec h3 { font-size: 1.12rem; font-weight: 700; margin:0; color:#0f172a; }
  .ob-sec p  { font-size:.86rem; color:#64748b; margin:.1rem 0 0; }

  /* KPI card */
  .kpi { background: #ffffff; border:1px solid #e2e8f0; border-radius: 14px;
         padding: 14px 16px 12px; box-shadow: 0 1px 2px rgba(15,23,42,.04);
         height: 100%; }
  .kpi .kpi-top { display:flex; align-items:center; gap:.4rem; }
  .kpi .dot { width:9px; height:9px; border-radius:50%; display:inline-block; }
  .kpi .kpi-label { font-size:.76rem; font-weight:600; color:#64748b;
                    text-transform:uppercase; letter-spacing:.04em; }
  .kpi .kpi-value { font-size:1.6rem; font-weight:800; color:#0f172a;
                    line-height:1.15; margin-top:.15rem;
                    font-variant-numeric: tabular-nums; }
  .kpi .kpi-sub   { font-size:.8rem; color:#94a3b8; margin-top:.05rem; }

  /* callout */
  .ob-note { border-left:3px solid #6366f1; background:#f5f3ff;
             padding:.6rem .85rem; border-radius:0 10px 10px 0;
             color:#3730a3; font-size:.9rem; margin:.2rem 0 .6rem; }

  /* tabs a touch bigger */
  button[data-baseweb="tab"] { font-size: .98rem; font-weight:600; }

  div[data-testid="stMetric"] { background:#fff; border:1px solid #e2e8f0;
        border-radius:12px; padding:12px 14px; }
</style>
"""

def inject_css() -> None:
    st.markdown(_CSS, unsafe_allow_html=True)


# ---- render helpers ---------------------------------------------------------
def hero(title: str, subtitle: str, chips: list[str] | None = None) -> None:
    chip_html = ''
    if chips:
        chip_html = '<div class="ob-chips">' + ''.join(
            f'<span class="ob-chip">{c}</span>' for c in chips) + '</div>'
    st.markdown(
        f'<div class="ob-hero"><h1>{title}</h1><p>{subtitle}</p>{chip_html}</div>',
        unsafe_allow_html=True)


def section(title: str, subtitle: str = '') -> None:
    sub = f'<p>{subtitle}</p>' if subtitle else ''
    st.markdown(f'<div class="ob-sec"><h3>{title}</h3>{sub}</div>',
                unsafe_allow_html=True)


def note(text: str) -> None:
    st.markdown(f'<div class="ob-note">{text}</div>', unsafe_allow_html=True)


def kpi(col, label: str, value: str, sub: str = '', accent: str = ACCENT) -> None:
    col.markdown(
        f'''<div class="kpi">
              <div class="kpi-top"><span class="dot" style="background:{accent}"></span>
              <span class="kpi-label">{label}</span></div>
              <div class="kpi-value">{value}</div>
              <div class="kpi-sub">{sub}</div>
            </div>''',
        unsafe_allow_html=True)


def show(fig: go.Figure, height: int | None = None, **kw) -> None:
    """Render a Plotly figure full-width, tolerant of streamlit version drift."""
    if height is not None:
        fig.update_layout(height=height)
    config = {'displayModeBar': False}
    try:
        st.plotly_chart(fig, width='stretch', config=config, **kw)
    except TypeError:
        st.plotly_chart(fig, use_container_width=True, config=config, **kw)


# ---- formatters -------------------------------------------------------------
def fmt_int(n) -> str:
    try:
        return f'{int(round(float(n))):,}'
    except (ValueError, TypeError):
        return str(n)


def fmt_ns(ns) -> str:
    try:
        ns = float(ns)
    except (ValueError, TypeError):
        return str(ns)
    if ns >= 1000:
        return f'{ns/1000:.2f} µs'
    return f'{ns:.0f} ns'


def fmt_rate(msgs_per_s: float) -> str:
    if msgs_per_s >= 1e6:
        return f'{msgs_per_s/1e6:.2f} M/s'
    if msgs_per_s >= 1e3:
        return f'{msgs_per_s/1e3:.1f} K/s'
    return f'{msgs_per_s:.0f}/s'


# ---- ITCH-file sidecar meta -------------------------------------------------
def meta_path(itch_path: str) -> str:
    return itch_path + '.meta.json'


def load_meta(itch_path: str) -> dict:
    p = meta_path(itch_path)
    if itch_path and os.path.exists(p):
        try:
            with open(p, 'r') as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}
    return {}


def units_to_dollars(units, price_ref):
    """Reverse the api_adapter normalisation.

    api_adapter maps a real dollar price to units as
        units = 500_000 + (price - ref) * 100        (1 unit = $0.01)
    so the inverse is price = ref + (units - 500_000)/100.
    When price_ref is None (synthetic / real-ITCH data already in the project's
    1/10000-dollar space) fall back to units/10000.
    """
    try:
        u = float(units)
    except (ValueError, TypeError):
        return 0.0
    if price_ref is None:
        return u / 10_000.0
    return float(price_ref) + (u - 500_000.0) / 100.0
