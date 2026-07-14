# Not a real strategy — exercises every VM opcode family (stateful indicators
# in both ring and raw variants, lagged series, all context scalars, the full
# arithmetic/logic set, and every config sink) for the golden parity test
# between backtest_project's refengine.py and sw/trade/dsl_vm.cpp.
param n = 14 in [2, 50]
param th = 60 in [40, 90]

let e    = ema(close, n)
let r    = rsi(close, n)
let a    = atr(n)
let hh   = highest(high, n)          # raw variant (series input)
let ll   = lowest(low, n)
let sd   = stddev(close, n)
let sm   = sma(close, n)
let hh2  = highest(e, n)             # ring variant (computed input)
let ll2  = lowest(e, n)
let sd2  = stddev(e, n)
let sm2  = sma(e, n)
let dl   = delay(e, 3)               # ring DELAY
let ch   = change(close)             # sugar: DELAY_RAW
let rc   = roc(close, n)
let lagc = close[2]                  # PUSH_SERIES_LAG
let neg  = -ch
let mixed = min(abs(neg), max(sqrt(sd), log(e))) + lagc / hh - ll * 0.0001

enter_long  when crossover(close, e) and r < th and not (position > 0)
exit_long   when crossunder(close, e) or mixed != mixed
enter_short when crossunder(r, th) and volume >= 0 and bar_index > n
exit_short  when crossover(r, th) or equity <= entry_price * 0 or hh2 == ll2 or sm2 <= 0 or dl < 0 or sd2 < 0 or rc != rc or sm < 0

set stop_loss   = 0.05
set take_profit = 0.1
set trail_stop  = 0.08
set size        = 0.5
