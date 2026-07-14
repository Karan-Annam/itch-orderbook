"""perf_panel.py — Performance tab: run the pipeline, compare the three engines."""
from __future__ import annotations
import os, sys, json, datetime
from pathlib import Path
import streamlit as st
import pandas as pd
import plotly.graph_objects as go

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))
from gui.panels import theme

ENGINES = ('map', 'simd', 'hw')
_PCTS   = [0.50, 0.90, 0.99, 0.999, 0.9999]
_PLABEL = ['p50', 'p90', 'p99', 'p99.9', 'p99.99']


def render(state: dict) -> None:
    _runner(state)
    run_dir = state.get('run_dir', '')
    if not run_dir or not os.path.isdir(run_dir):
        return

    present = [e for e in ENGINES if not _hist(run_dir, e).empty]
    if not present:
        st.warning('No latency CSVs found in the run directory.')
        return

    _methodology()
    st.divider();  _scoreboard(run_dir, present)
    st.divider();  _percentile_chart(run_dir, present)
    st.divider();  _distribution(run_dir, present)
    st.divider();  _per_type(run_dir, present)
    st.divider();  _rtl_counters(run_dir)


# ---- pipeline runner --------------------------------------------------------
def _runner(state: dict) -> None:
    theme.section('Run the three-engine pipeline',
                  'Builds (if needed) and replays your feed through std::map, '
                  'the SIMD book, and the Verilated RTL — writing one latency '
                  'histogram per engine.')
    itch = state.get('itch_file', '')
    if not itch or not os.path.exists(itch):
        st.info('Load a feed on the **Data** tab first.', icon='📂')
        return

    meta = theme.load_meta(itch)
    c1, c2 = st.columns([3, 1])
    c1.caption(f"Feed: **{meta.get('label') or Path(itch).name}**  ·  `{itch}`")
    go_run = c2.button('▶  Run pipeline', type='primary', use_container_width=True)

    if go_run:
        ts = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
        run_dir = str(ROOT / 'gui' / 'runs' / ts)
        os.makedirs(run_dir, exist_ok=True)
        script = f'bash "{ROOT}/gui/run_pipeline.sh" "{itch}" "{run_dir}"'
        from gui.subprocess_runner import run_bash
        rc, _ = run_bash(script, status_label='Running pipeline…', cwd=str(ROOT))
        if rc == 0:
            state['run_dir'] = run_dir
            st.session_state['run_dir'] = run_dir
            st.success('Pipeline complete.', icon='✅')
        else:
            st.error('Pipeline failed — expand the log above for the error.')


def _methodology() -> None:
    with st.expander('How these latencies are measured  —  and what to trust'):
        st.markdown(
            '**What each number is**\n'
            '- **C++ std::map / SIMD** — `rdtscp` around **decode + book update** '
            'per message (compute only, no I/O). This is *measured* wall-clock time '
            'on your CPU. Median = typical cost; the far tail is host OS jitter '
            '(scheduler preemptions), not the algorithm.\n'
            '- **RTL hardware** — **cycles** from a message\'s first input byte to '
            'its commit in the Verilated 128-bit pipeline. Cycles are '
            'architecturally real; ns = cycles ÷ implemented clock.\n'
            '- **RTL throughput** — reported separately as *service time* '
            '(commit-to-commit), which **is** ingest-bound (≈200 ns/msg → 5 M msg/s).\n\n'
            '**What to trust (important)**\n'
            '- Verilator is **cycle-accurate, not timing-accurate.** It tells you '
            '*how many cycles* an op takes — not what clock the design can run at.\n'
            '- The default **100 MHz** is the implemented Spartan-7 constraint. '
            'Cycle counts remain portable; absolute nanoseconds depend on the '
            'target and post-route timing result.')





# ---- data loading -----------------------------------------------------------
def _hist(run_dir: str, engine: str, typ: str = 'all') -> pd.DataFrame:
    p = os.path.join(run_dir, f'latency_{engine}_{typ}.csv')
    if not os.path.exists(p):
        return pd.DataFrame()
    try:
        frame = pd.read_csv(p)
        if 'latency_cycles' in frame.columns and 'latency_ns' not in frame.columns:
            mhz = _counters(run_dir, engine).get('clock_mhz', 0)
            if mhz <= 0:
                return pd.DataFrame()
            frame['latency_ns'] = frame['latency_cycles'] * (1000.0 / mhz)
        return frame
    except Exception:
        return pd.DataFrame()


def _pct(df: pd.DataFrame, p: float) -> float:
    if df.empty:
        return float('nan')
    d = df.sort_values('latency_ns')
    total = d['count'].sum()
    if total <= 0:
        return float('nan')
    cum = d['count'].cumsum()
    hit = d[cum >= p * total]
    return float(hit['latency_ns'].iloc[0]) if not hit.empty else float(d['latency_ns'].max())


def _counters(run_dir: str, engine: str) -> dict:
    p = os.path.join(run_dir, f'perf_counters_{engine}.csv')
    if not os.path.exists(p):
        return {}
    try:
        df = pd.read_csv(p)
        return {str(r['counter']): float(r['value']) for _, r in df.iterrows()}
    except Exception:
        return {}


# ---- scoreboard -------------------------------------------------------------
def _scoreboard(run_dir: str, present: list[str]) -> None:
    theme.section('Scoreboard',
                  'Median compute/pipeline latency and how far the tail strays from it. '
                  'Lower median and flatter tail are both better.')
    stats = {}
    for e in present:
        h = _hist(run_dir, e)
        p50 = _pct(h, 0.50); p99 = _pct(h, 0.99); p9999 = _pct(h, 0.9999)
        stats[e] = {'p50': p50, 'p99': p99, 'p9999': p9999,
                    'tail': (p9999 / p50) if p50 else float('nan')}

    fastest   = min(present, key=lambda e: stats[e]['p50'])
    steadiest = min(present, key=lambda e: stats[e]['tail'])

    cols = st.columns(len(present))
    for col, e in zip(cols, present):
        s = stats[e]
        badge = ''
        if e == fastest:   badge += ' ⚡'
        if e == steadiest: badge += ' 🎯'
        theme.kpi(col, theme.ENGINE_LABEL[e] + badge,
                  theme.fmt_ns(s['p50']),
                  f"p99 {theme.fmt_ns(s['p99'])} · tail {s['tail']:.0f}× median",
                  theme.ENGINE_COLOR[e])

    fj, sj = stats[fastest], stats[steadiest]
    worst_tail = max(stats[e]['tail'] for e in present)
    theme.note(
        f"⚡ <b>{theme.ENGINE_LABEL[fastest]}</b> has the lowest median "
        f"({theme.fmt_ns(fj['p50'])}).  🎯 <b>{theme.ENGINE_LABEL[steadiest]}</b> "
        f"is the most deterministic — its p99.99 is only "
        f"<b>{sj['tail']:.0f}×</b> its median, versus up to "
        f"<b>{worst_tail:.0f}×</b> for software (OS jitter). "
        f"Bounded tails are the whole point of putting the book in silicon.")

    hw = _counters(run_dir, 'hw')
    thr = hw.get('throughput_msg_s')
    if thr:
        st.caption(
            f"RTL sustained throughput ≈ **{thr/1e6:.1f} M msg/s** "
            f"(median service time {hw.get('service_p50_cycles', 0):.0f} cycles; "
            "a separate number from end-to-end latency above). "
            "Software throughput isn't shown: its timer measures compute only, "
            "with no I/O modelled.")


# ---- percentile curve (the tail story) --------------------------------------
def _percentile_chart(run_dir: str, present: list[str]) -> None:
    theme.section('Latency across percentiles',
                  'Compute latency (SW) vs pipeline latency (RTL), ingest excluded. '
                  'Software climbs steeply into the tail; hardware stays bounded.')
    fig = go.Figure()
    for e in present:
        h = _hist(run_dir, e)
        ys = [_pct(h, p) for p in _PCTS]
        fig.add_trace(go.Scatter(
            x=_PLABEL, y=ys, name=theme.ENGINE_LABEL[e], mode='lines+markers',
            line=dict(color=theme.ENGINE_COLOR[e], width=3),
            marker=dict(size=8),
            hovertemplate=f'{theme.ENGINE_LABEL[e]} · %{{x}} = %{{y:,.0f}} ns<extra></extra>'))
    fig.update_layout(title='Latency by percentile (log scale)', height=430,
                      yaxis=dict(type='log', title='latency (ns)'),
                      xaxis_title='percentile')
    theme.show(fig)


# ---- distribution -----------------------------------------------------------
def _distribution(run_dir: str, present: list[str]) -> None:
    theme.section('Latency distribution', 'The bulk of the messages, normalised to a share of the stream.')
    xcap = max(_pct(_hist(run_dir, e), 0.95) for e in present) * 1.6
    xcap = max(xcap, 300)
    fig = go.Figure()
    for e in present:
        h = _hist(run_dir, e).sort_values('latency_ns')
        if h.empty:
            continue
        tot = h['count'].sum()
        fig.add_trace(go.Bar(
            x=h['latency_ns'], y=h['count'] / tot,
            name=theme.ENGINE_LABEL[e],
            marker=dict(color=theme.ENGINE_COLOR[e]), opacity=0.55,
            hovertemplate=f'{theme.ENGINE_LABEL[e]} · %{{x}} ns = %{{y:.1%}}<extra></extra>'))
    fig.update_layout(title='Per-message latency (share of stream)', height=380,
                      barmode='overlay', xaxis=dict(range=[0, xcap], title='latency (ns)'),
                      yaxis=dict(title='share', tickformat='.0%'))
    theme.show(fig)
    st.caption(f'x-axis capped at {xcap:,.0f} ns to show the body of the '
               'distribution; rare software outliers extend well beyond.')

    _percentile_table(run_dir, present)


def _percentile_table(run_dir: str, present: list[str]) -> None:
    head = ''.join(
        f'<th style="text-align:right;padding:6px 12px;color:{theme.ENGINE_COLOR[e]}">'
        f'{theme.ENGINE_LABEL[e]}</th>' for e in present)
    rows = ''
    data = {e: _hist(run_dir, e) for e in present}
    for lbl, p in zip(_PLABEL, _PCTS):
        cells = ''
        vals = {e: _pct(data[e], p) for e in present}
        best = min(vals.values())
        for e in present:
            v = vals[e]
            strong = 'font-weight:700;' if v == best else ''
            cells += (f'<td style="text-align:right;padding:6px 12px;'
                      f'font-variant-numeric:tabular-nums;{strong}">'
                      f'{theme.fmt_ns(v)}</td>')
        rows += (f'<tr><td style="padding:6px 12px;color:#64748b;font-weight:600">'
                 f'{lbl}</td>{cells}</tr>')
    st.markdown(
        f'<table style="border-collapse:collapse;width:100%;font-size:.9rem">'
        f'<thead><tr style="border-bottom:2px solid #e2e8f0">'
        f'<th style="text-align:left;padding:6px 12px;color:#64748b">percentile</th>'
        f'{head}</tr></thead><tbody>{rows}</tbody></table>',
        unsafe_allow_html=True)
    st.caption('Bold = fastest engine at that percentile. Note how the software '
               'gap widens as you move into the tail.')


# ---- per message type -------------------------------------------------------
def _per_type(run_dir: str, present: list[str]) -> None:
    theme.section('Latency by message type', 'Which operations cost the most.')
    all_types = ['Add', 'Execute', 'Cancel', 'Delete', 'Replace', 'Trade']
    types = []
    for t in all_types:
        if any(not _hist(run_dir, e, t).empty for e in present):
            types.append(t)
    if not types:
        st.caption('No per-type histograms in this run.')
        return
    fig = go.Figure()
    for e in present:
        ys = [_pct(_hist(run_dir, e, t), 0.50) for t in types]
        fig.add_trace(go.Bar(x=types, y=ys, name=theme.ENGINE_LABEL[e],
                             marker=dict(color=theme.ENGINE_COLOR[e]),
                             hovertemplate=f'{theme.ENGINE_LABEL[e]} · %{{x}} '
                                           'p50 = %{y:,.0f} ns<extra></extra>'))
    fig.update_layout(title='Median latency by message type', height=380,
                      barmode='group', xaxis_title='message type',
                      yaxis_title='p50 latency (ns)')
    theme.show(fig)
    st.caption('Deletes and Replaces are heaviest — emptying a level triggers a '
               'best-price rescan. Trades touch no resting orders.')


# ---- RTL hardware counters --------------------------------------------------
def _rtl_counters(run_dir: str) -> None:
    c = _counters(run_dir, 'hw')
    if not c:
        return
    theme.section('Inside the RTL — cycles are the real metric',
                  'Cycle counts are architecturally exact; the default conversion '
                  'uses the implemented 100 MHz Spartan-7 clock.')
    sim_mhz = c.get('clock_mhz', 100) or 100
    ns_per_cyc_sim = 1000.0 / sim_mhz

    # Per-op latency in CYCLES (invariant) from the per-type ns histograms.
    def p50_cycles(typ: str) -> float:
        ns = _pct(_hist(run_dir, 'hw', typ), 0.50)
        return ns / ns_per_cyc_sim if ns_per_cyc_sim else 0
    all_cyc   = _pct(_hist(run_dir, 'hw'), 0.50) / ns_per_cyc_sim
    tail_cyc  = _pct(_hist(run_dir, 'hw'), 0.9999) / ns_per_cyc_sim

    k = st.columns(4)
    theme.kpi(k[0], 'Add', f'{p50_cycles("Add"):.0f} cyc', 'decode + insert', theme.ENGINE_COLOR['hw'])
    theme.kpi(k[1], 'Delete', f'{p50_cycles("Delete"):.0f} cyc', 'incl. best rescan', '#2563eb')
    theme.kpi(k[2], 'Trade', f'{p50_cycles("Trade"):.0f} cyc', 'no book change', '#059669')
    theme.kpi(k[3], 'p99.99 (worst)', f'{tail_cyc:.0f} cyc', 'bounded by scan depth', '#f59e0b')

    # Interactive: the same cycle count, scaled to whatever clock you assume.
    st.write('')
    assumed = st.slider('Explore cycle conversion at a clock frequency (MHz)',
                        100, 1000, int(sim_mhz), 50)
    ns_at = all_cyc * (1000.0 / assumed)
    st.markdown(
        f"At **{assumed} MHz**, the median op ({all_cyc:.0f} cycles) would be "
        f"**{ns_at:.0f} ns**. The cycle count never changed — only the label did. "
        f"That's exactly why ns is provisional until synthesis pins the real Fmax.")

    st.write('')
    k2 = st.columns(3)
    theme.kpi(k2[0], 'Total cycles', theme.fmt_int(c.get('total_cycles', 0)),
              f"{theme.fmt_int(c.get('msg_total', 0))} msgs", theme.MUTED)
    theme.kpi(k2[1], 'Best-price rescans', theme.fmt_int(c.get('scan_count', 0)),
              f"{theme.fmt_int(c.get('scan_cycles_total',0))} cycles total", '#f59e0b')
    service_cyc = c.get('service_p50_cycles', 0)
    theme.kpi(k2[2], 'Service time', f"{service_cyc:.0f} cycles/msg",
              f"{service_cyc * ns_per_cyc_sim:.0f} ns at {sim_mhz:.0f} MHz; "
              f"≈ {c.get('throughput_msg_s',0)/1e6:.1f} M msg/s", theme.MUTED)

    p1, p2, pg = c.get('hash_probe_1', 0), c.get('hash_probe_2', 0), c.get('hash_probe_gt2', 0)
    tot = p1 + p2 + pg
    if tot:
        st.write('')
        fig = go.Figure(go.Bar(
            x=['1 probe', '2 probes', '3+ probes'], y=[p1, p2, pg],
            marker=dict(color=['#059669', '#f59e0b', '#ef4444']),
            text=[f'{v/tot*100:.1f}%' for v in (p1, p2, pg)], textposition='auto'))
        fig.update_layout(title='Order-ref hash: probe depth', height=300,
                          yaxis_title='lookups')
        theme.show(fig)
        st.caption(f'{p1/tot*100:.1f}% of order-ref lookups resolve on the first '
                   'probe — the open-addressing hash table stays shallow.')
