"""Order Book Lab: interactive NASDAQ ITCH 5.0 order-book dashboard.

A three-stage workflow:
  1. Data        pick a source (synthetic / real ITCH / live API) -> project .itch
  2. Order Book  replay it and watch the book: depth ladder, market depth, spread
  3. Performance run the same feed through three engines (std::map / SIMD / RTL)
                 and compare latency distributions, tails, and throughput.

Run from the orderbook_project/ directory:
  streamlit run gui/app.py
"""
import os, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import streamlit as st
from gui.panels import theme

st.set_page_config(page_title='Order Book Lab', page_icon='📈',
                   layout='wide', initial_sidebar_state='expanded')
theme.install_template()
theme.inject_css()

# ---- session state ----------------------------------------------------------
if 'app_state' not in st.session_state:
    st.session_state['app_state'] = {'itch_file': '', 'run_dir': ''}
state = st.session_state['app_state']
if 'run_dir' in st.session_state:            # restored after a pipeline run
    state['run_dir'] = st.session_state['run_dir']


def _msg_count(path: str) -> int:
    """Cheap message count for the status card."""
    import struct
    n = 0
    try:
        with open(path, 'rb') as f:
            while True:
                h = f.read(2)
                if len(h) < 2:
                    break
                blen = struct.unpack('>H', h)[0]
                if blen == 0:
                    break
                if len(f.read(blen)) < blen:
                    break
                n += 1
    except OSError:
        return 0
    return n


# ---- sidebar: pipeline status ----------------------------------------------
with st.sidebar:
    st.markdown('### 📈 Order Book Lab')
    st.caption('NASDAQ ITCH 5.0 · software vs hardware')
    st.divider()

    itch = state.get('itch_file', '')
    if itch and os.path.exists(itch):
        meta = theme.load_meta(itch)
        st.markdown('**Loaded feed**')
        st.success(meta.get('label') or Path(itch).name, icon='✅')
        c1, c2 = st.columns(2)
        c1.metric('Messages', theme.fmt_int(_msg_count(itch)))
        c2.metric('Size', f'{os.path.getsize(itch)/1024:.0f} KB')
    else:
        st.info('No feed loaded.\nStart on the **Data** tab.', icon='📂')

    st.divider()
    run_dir = state.get('run_dir', '')
    if run_dir and os.path.isdir(run_dir):
        n_csv = len([f for f in os.listdir(run_dir) if f.endswith('.csv')])
        st.markdown('**Pipeline**')
        st.success(f'Results ready · {n_csv} CSVs', icon='⚡')
        st.caption(Path(run_dir).name)
    else:
        st.markdown('**Pipeline**')
        st.caption('Not run yet — see the **Performance** tab.')

    st.divider()
    st.caption('Three engines compared:')
    for e in ('map', 'simd', 'hw'):
        st.markdown(
            f"<span style='display:inline-block;width:10px;height:10px;border-radius:50%;"
            f"background:{theme.ENGINE_COLOR[e]};margin-right:7px'></span>"
            f"<b>{theme.ENGINE_LABEL[e]}</b> "
            f"<span style='color:#94a3b8;font-size:.85em'>{theme.ENGINE_LONG[e]}</span>",
            unsafe_allow_html=True)

# ---- hero -------------------------------------------------------------------
theme.hero(
    'Order Book Lab',
    'Replay real market data through a NASDAQ ITCH 5.0 limit order book — '
    'then race three implementations of it, from std::map to silicon.',
    chips=['ITCH 5.0 feed handler', 'std::map reference',
           'Direct-indexed + AVX2 SIMD', 'SystemVerilog RTL @ 250 MHz',
           'Verilator cycle-accurate'],
)
st.write('')

# ---- tabs -------------------------------------------------------------------
tab_data, tab_book, tab_perf = st.tabs(
    ['📂  Data', '📊  Order Book', '⚡  Performance'])

with tab_data:
    from gui.panels.data_panel import render as render_data
    render_data(state)

with tab_book:
    from gui.panels.book_panel import render as render_book
    render_book(state)

with tab_perf:
    from gui.panels.perf_panel import render as render_perf
    render_perf(state)
