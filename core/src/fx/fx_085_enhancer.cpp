// 85 Enhancer (ctor FUN_1003c990, vtable 0x100a98b4: setSampleRate FUN_1003c9b0, process FUN_1003ca50,
// setParam FUN_1003cc00, paramName 0x1003ccf0, paramDisplay 0x1003cdc0; init/tempo 0x10002170 is a no-op).
// Hi part: per channel a 2-pole state-variable filter (f = (15000 + 45000*p2)/sr, damping 0.6 + 1.2*(1-2p2+p2^2))
// whose high-pass/notch mix (k = 1-p2) is added to the dry signal scaled by exp(p0)-1.
// Lo part: the mono sum through two cascaded one-poles (coef (400 + 1600*p4^2)/sr); (lp2 - 0.15*lp1)*(exp(1.6*p3)-1)
// is added to both channels (computed from the previous sample's state).
// The original keeps intermediates on the x87 stack (53-bit precision control); mirrored with double.
// Oracle-verified: bit-exact (random 8 + extremes).
#include "effect_base.h"

namespace hs2 {
namespace {

using X = double;  // the original runs the x87 at 53-bit precision (MSVC default control word)

class Enhancer : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.5);  // +0xc tail: trunc(sr * 0.5)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        for (auto& c : ch_) c = Ch{};
        lo1_ = lo2_ = 0.f;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        const float m2 = float(X(k_) * X(-2.0f));
        for (int i = 0; i < n; ++i) {
            const X lo = (X(lo2_) - X(lo1_) * X(0.15f)) * X(loGain_);
            X xs[2];
            for (int c = 0; c < 2; ++c) {
                Ch& s = ch_[c];
                const X x = io[2 * i + c];
                xs[c] = x;
                const X hp = ((x + X(s.xprev)) - X(s.lp)) - X(damp_) * X(s.bp) + X(1e-8f);
                const X bp = hp * X(f_) + X(s.bp);
                s.bp = float(bp);
                const X lp = bp * X(f_) + X(s.lp);
                s.lp = float(lp);
                s.xprev = float(x);
                const X y = (bp * X(m2) + (((x + x) - lp) - hp) * X(k_) + hp) * X(hiGain_) + x + lo;
                io[2 * i + c] = float(y);
            }
            const X a = ((xs[1] + xs[0]) - X(lo1_)) * X(loC_) + X(lo1_);
            lo1_ = float(a);
            lo2_ = float((a - X(lo2_)) * X(loC_) + X(lo2_));
        }
    }

private:
    void onParam(int i, float v) override {
        const X p = v;
        switch (i) {
            case 0: hiGain_ = float(std::exp(p) - 1.0L); return;
            case 2:
                f_ = float((p * X(45000.f) + X(15000.f)) * X(invSr_));
                damp_ = float((((X(1.f) - p) - p) + p * p) * X(1.2f) + X(0.6f));
                k_ = float(X(1.f) - p);
                return;
            case 3: loGain_ = float(std::exp(p * X(1.6f)) - 1.0L); return;
            case 4: loC_ = float((p * p * X(1600.f) + X(400.f)) * X(invSr_)); return;
            default: return;
        }
    }
    struct Ch {
        float bp = 0, lp = 0, xprev = 0;
    };
    Ch ch_[2];
    float lo1_ = 0, lo2_ = 0;
    float hiGain_ = 0, k_ = 0, f_ = 0, damp_ = 0, loGain_ = 0, loC_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_85() { return std::make_unique<Enhancer>(); }

}  // namespace hs2
