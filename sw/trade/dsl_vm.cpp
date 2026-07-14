// Compiled with -ffp-contract=off (Makefile) — see header for why.
#include "dsl_vm.hpp"

#include <cmath>
#include <stdexcept>

namespace ob::trade {

StrategyVM::StrategyVM(const Program& prog, std::vector<float> params)
    : prog_(prog),
      params_(params.empty() ? prog.param_defaults : std::move(params)),
      state_(prog.state_floats, 0.0f),
      locals_(prog.n_locals > 0 ? prog.n_locals : 1, 0.0f) {
    if (params_.size() != prog.param_defaults.size())
        throw std::runtime_error("StrategyVM: wrong number of params");
}

Signals StrategyVM::step(const BarSeries& bars, int t, const VmCtx& ctx) {
    const float* o = bars.o.data();
    const float* h = bars.h.data();
    const float* l = bars.l.data();
    const float* c = bars.c.data();
    const float* v = bars.v.data();
    float* state = state_.data();
    float* locals = locals_.data();

    float stack[VM_MAX_STACK];
    int sp = 0;
    Signals sig;

    const int n_code = (int)prog_.code.size();
    for (int pc = 0; pc < n_code; ++pc) {
        uint32_t ins = prog_.code[pc];
        int op = (int)(ins >> 24);
        int arg = (int)(ins & 0xFFFFFF);
        switch (op) {
        case OP_PUSH_CONST:  stack[sp++] = prog_.consts[arg]; break;
        case OP_PUSH_PARAM:  stack[sp++] = params_[arg]; break;
        case OP_PUSH_SERIES: {
            const float* s = (arg == 0) ? o : (arg == 1) ? h
                            : (arg == 2) ? l : (arg == 3) ? c : v;
            stack[sp++] = s[t];
            break;
        }
        case OP_PUSH_SERIES_LAG: {
            int sid = arg >> 16, lag = arg & 0xFFFF;
            const float* s = (sid == 0) ? o : (sid == 1) ? h
                            : (sid == 2) ? l : (sid == 3) ? c : v;
            int idx = t - lag;
            stack[sp++] = s[idx > 0 ? idx : 0];
            break;
        }
        case OP_PUSH_CTX:
            stack[sp++] = (arg == 0) ? (float)t : (arg == 1) ? ctx.position
                         : (arg == 2) ? ctx.entry_px : ctx.equity;
            break;
        case OP_LOAD:  stack[sp++] = locals[arg]; break;
        case OP_STORE: locals[arg] = stack[--sp]; break;
        case OP_ADD: { float y = stack[--sp]; stack[sp - 1] = stack[sp - 1] + y; break; }
        case OP_SUB: { float y = stack[--sp]; stack[sp - 1] = stack[sp - 1] - y; break; }
        case OP_MUL: { float y = stack[--sp]; stack[sp - 1] = stack[sp - 1] * y; break; }
        case OP_DIV: {
            float y = stack[--sp];
            stack[sp - 1] = (y != 0.0f) ? stack[sp - 1] / y : 0.0f;
            break;
        }
        case OP_NEG: stack[sp - 1] = -stack[sp - 1]; break;
        case OP_ABS: stack[sp - 1] = fabsf(stack[sp - 1]); break;
        case OP_MIN2: { float y = stack[--sp]; float x = stack[sp - 1];
                        stack[sp - 1] = (x < y) ? x : y; break; }
        case OP_MAX2: { float y = stack[--sp]; float x = stack[sp - 1];
                        stack[sp - 1] = (x > y) ? x : y; break; }
        case OP_SQRT: { float x = stack[sp - 1];
                        stack[sp - 1] = (x > 0.0f) ? sqrtf(x) : 0.0f; break; }
        case OP_LOG:  { float x = stack[sp - 1];
                        stack[sp - 1] = (x > 0.0f) ? logf(x) : 0.0f; break; }
        case OP_GT: { float y = stack[--sp]; stack[sp - 1] = (stack[sp - 1] > y) ? 1.0f : 0.0f; break; }
        case OP_LT: { float y = stack[--sp]; stack[sp - 1] = (stack[sp - 1] < y) ? 1.0f : 0.0f; break; }
        case OP_GE: { float y = stack[--sp]; stack[sp - 1] = (stack[sp - 1] >= y) ? 1.0f : 0.0f; break; }
        case OP_LE: { float y = stack[--sp]; stack[sp - 1] = (stack[sp - 1] <= y) ? 1.0f : 0.0f; break; }
        case OP_EQ: { float y = stack[--sp]; stack[sp - 1] = (stack[sp - 1] == y) ? 1.0f : 0.0f; break; }
        case OP_NE: { float y = stack[--sp]; stack[sp - 1] = (stack[sp - 1] != y) ? 1.0f : 0.0f; break; }
        case OP_AND: { float y = stack[--sp];
                       stack[sp - 1] = (stack[sp - 1] != 0.0f && y != 0.0f) ? 1.0f : 0.0f; break; }
        case OP_OR:  { float y = stack[--sp];
                       stack[sp - 1] = (stack[sp - 1] != 0.0f || y != 0.0f) ? 1.0f : 0.0f; break; }
        case OP_NOT: stack[sp - 1] = (stack[sp - 1] == 0.0f) ? 1.0f : 0.0f; break;

        case OP_EMA: {
            float n = stack[--sp];
            float x = stack[--sp];
            float* st = state + prog_.state_off[arg];   // [count, value]
            int n_eff = (int)n; if (n_eff < 1) n_eff = 1;
            if (st[0] == 0.0f) {
                st[1] = x;
            } else {
                float alpha = 2.0f / (float)(n_eff + 1);
                st[1] = st[1] + alpha * (x - st[1]);
            }
            st[0] = st[0] + 1.0f;
            stack[sp++] = st[1];
            break;
        }
        case OP_RSI: {
            float n = stack[--sp];
            float x = stack[--sp];
            float* st = state + prog_.state_off[arg];   // [count, prev_x, ag, al]
            int n_eff = (int)n; if (n_eff < 1) n_eff = 1;
            int count = (int)st[0];
            float res;
            if (count == 0) {
                res = 50.0f;
            } else {
                float delta = x - st[1];
                float g = (delta > 0.0f) ? delta : 0.0f;
                float lo = (-delta > 0.0f) ? -delta : 0.0f;
                if (count == 1) {
                    st[2] = g;
                    st[3] = lo;
                } else {
                    st[2] = st[2] + (g - st[2]) / (float)n_eff;
                    st[3] = st[3] + (lo - st[3]) / (float)n_eff;
                }
                float denom = st[2] + st[3];
                res = (denom == 0.0f) ? 50.0f : 100.0f * (st[2] / denom);
            }
            st[1] = x;
            st[0] = (float)(count + 1);
            stack[sp++] = res;
            break;
        }
        case OP_ATR: {
            float n = stack[--sp];
            float* st = state + prog_.state_off[arg];   // [count, value]
            int n_eff = (int)n; if (n_eff < 1) n_eff = 1;
            float tr;
            if (t == 0) {
                tr = h[t] - l[t];
            } else {
                float pc_ = c[t - 1];
                float a = h[t] - l[t];
                float b1 = fabsf(h[t] - pc_);
                float c1 = fabsf(l[t] - pc_);
                float m = (b1 > c1) ? b1 : c1;
                tr = (a > m) ? a : m;
            }
            if (st[0] == 0.0f) {
                st[1] = tr;
            } else {
                st[1] = st[1] + (tr - st[1]) / (float)n_eff;
            }
            st[0] = st[0] + 1.0f;
            stack[sp++] = st[1];
            break;
        }
        case OP_SMA: case OP_HIGHEST: case OP_LOWEST: case OP_STDDEV: {
            float n = stack[--sp];
            float x = stack[--sp];
            float* st = state + prog_.state_off[arg];   // [count, ring...]
            int cap = prog_.state_cap[arg];
            int n_eff = (int)n;
            if (n_eff < 1) n_eff = 1;
            if (n_eff > cap) n_eff = cap;
            int count = (int)st[0];
            st[1 + count % cap] = x;
            count += 1;
            st[0] = (float)count;
            int k = (count < n_eff) ? count : n_eff;
            int base = count - k;
            if (op == OP_SMA) {
                float acc = 0.0f;
                for (int i = 0; i < k; ++i) acc = acc + st[1 + (base + i) % cap];
                stack[sp++] = acc / (float)k;
            } else if (op == OP_HIGHEST) {
                float acc = st[1 + base % cap];
                for (int i = 1; i < k; ++i) {
                    float xi = st[1 + (base + i) % cap];
                    if (xi > acc) acc = xi;
                }
                stack[sp++] = acc;
            } else if (op == OP_LOWEST) {
                float acc = st[1 + base % cap];
                for (int i = 1; i < k; ++i) {
                    float xi = st[1 + (base + i) % cap];
                    if (xi < acc) acc = xi;
                }
                stack[sp++] = acc;
            } else {  // STDDEV, two-pass
                float acc = 0.0f;
                for (int i = 0; i < k; ++i) acc = acc + st[1 + (base + i) % cap];
                float mean = acc / (float)k;
                float ss = 0.0f;
                for (int i = 0; i < k; ++i) {
                    float d = st[1 + (base + i) % cap] - mean;
                    ss = ss + d * d;
                }
                float var = ss / (float)k;
                stack[sp++] = (var > 0.0f) ? sqrtf(var) : 0.0f;
            }
            break;
        }
        case OP_SMA_RAW: case OP_HIGHEST_RAW: case OP_LOWEST_RAW:
        case OP_STDDEV_RAW: {
            float n = stack[--sp];
            int cap = prog_.state_cap[arg];
            int aux = prog_.state_aux[arg];
            int sid = aux >> 16, lag = aux & 0xFFFF;
            const float* s = (sid == 0) ? o : (sid == 1) ? h
                            : (sid == 2) ? l : (sid == 3) ? c : v;
            int n_eff = (int)n;
            if (n_eff < 1) n_eff = 1;
            if (n_eff > cap) n_eff = cap;
            int k = (t + 1 < n_eff) ? t + 1 : n_eff;
            int j0 = t - k + 1;
            if (op == OP_SMA_RAW) {
                float acc = 0.0f;
                for (int j = j0; j <= t; ++j) {
                    int idx = j - lag;
                    acc = acc + s[idx > 0 ? idx : 0];
                }
                stack[sp++] = acc / (float)k;
            } else if (op == OP_HIGHEST_RAW) {
                int idx0 = j0 - lag;
                float acc = s[idx0 > 0 ? idx0 : 0];
                for (int j = j0 + 1; j <= t; ++j) {
                    int idx = j - lag;
                    float xi = s[idx > 0 ? idx : 0];
                    if (xi > acc) acc = xi;
                }
                stack[sp++] = acc;
            } else if (op == OP_LOWEST_RAW) {
                int idx0 = j0 - lag;
                float acc = s[idx0 > 0 ? idx0 : 0];
                for (int j = j0 + 1; j <= t; ++j) {
                    int idx = j - lag;
                    float xi = s[idx > 0 ? idx : 0];
                    if (xi < acc) acc = xi;
                }
                stack[sp++] = acc;
            } else {  // STDDEV_RAW, two-pass
                float acc = 0.0f;
                for (int j = j0; j <= t; ++j) {
                    int idx = j - lag;
                    acc = acc + s[idx > 0 ? idx : 0];
                }
                float mean = acc / (float)k;
                float ss = 0.0f;
                for (int j = j0; j <= t; ++j) {
                    int idx = j - lag;
                    float d = s[idx > 0 ? idx : 0] - mean;
                    ss = ss + d * d;
                }
                float var = ss / (float)k;
                stack[sp++] = (var > 0.0f) ? sqrtf(var) : 0.0f;
            }
            break;
        }
        case OP_DELAY_RAW: {
            float kk = stack[--sp];
            int cap = prog_.state_cap[arg];
            int aux = prog_.state_aux[arg];
            int sid = aux >> 16, lag = aux & 0xFFFF;
            const float* s = (sid == 0) ? o : (sid == 1) ? h
                            : (sid == 2) ? l : (sid == 3) ? c : v;
            int k_eff = (int)kk;
            if (k_eff < 0) k_eff = 0;
            if (k_eff > cap - 1) k_eff = cap - 1;
            int avail = (t + 1 < cap) ? t + 1 : cap;
            int back = (k_eff < avail - 1) ? k_eff : avail - 1;
            int idx = t - back - lag;
            stack[sp++] = s[idx > 0 ? idx : 0];
            break;
        }
        case OP_DELAY: {
            float kk = stack[--sp];
            float x = stack[--sp];
            float* st = state + prog_.state_off[arg];
            int cap = prog_.state_cap[arg];
            int count = (int)st[0];
            st[1 + count % cap] = x;
            count += 1;
            st[0] = (float)count;
            int k_eff = (int)kk;
            if (k_eff < 0) k_eff = 0;
            if (k_eff > cap - 1) k_eff = cap - 1;
            int avail = (count < cap) ? count : cap;
            int back = (k_eff < avail - 1) ? k_eff : avail - 1;
            stack[sp++] = st[1 + (count - 1 - back) % cap];
            break;
        }
        case OP_CROSSOVER: case OP_CROSSUNDER: {
            float y = stack[--sp];
            float x = stack[--sp];
            float* st = state + prog_.state_off[arg];   // [count, pa, pb]
            int count = (int)st[0];
            bool r;
            if (op == OP_CROSSOVER)
                r = count >= 1 && x > y && st[1] <= st[2];
            else
                r = count >= 1 && x < y && st[1] >= st[2];
            st[1] = x;
            st[2] = y;
            st[0] = (float)(count + 1);
            stack[sp++] = r ? 1.0f : 0.0f;
            break;
        }
        case OP_SIG_EL: sig.el = sig.el || (stack[--sp] != 0.0f); break;
        case OP_SIG_XL: sig.xl = sig.xl || (stack[--sp] != 0.0f); break;
        case OP_SIG_ES: sig.es = sig.es || (stack[--sp] != 0.0f); break;
        case OP_SIG_XS: sig.xs = sig.xs || (stack[--sp] != 0.0f); break;
        case OP_SET_STOP:  sig.stop = stack[--sp]; break;
        case OP_SET_TP:    sig.tp = stack[--sp]; break;
        case OP_SET_TRAIL: sig.trail = stack[--sp]; break;
        case OP_SET_SIZE: {
            float s = stack[--sp];
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            sig.size = s;
            break;
        }
        case OP_HALT: pc = n_code; break;
        default: break;
        }
    }
    return sig;
}

}  // namespace ob::trade
