// 92 Multiband (ctor FUN_1003dc50, vtable 0x100a9988: setSampleRate FUN_1003dc70, process FUN_1003dd10,
// setParam FUN_1003df40, paramName 0x1003e090, paramDisplay 0x1003e1a0; init/tempo 0x10002170 is a no-op).
// Three bands from two one-pole low-passes (18000/sr on the input, then 250/sr): hi = x - lp1, mid = lp1 - lp2,
// lo = lp2. Each band has a stereo-linked |L+R| follower (attack p5: 1 - exp(-exp(9-4p)/sr),
// release p6: 1 - exp(-exp(3.6-2.6p)/sr)) and gain g_band / (env*amount + 1), amount = p2^3*30.
// Band gains: e = 2*exp(4.6*p0 + 2.4*p2 - 2.3); hi = e*p3, mid = e*p4/2, lo = e - e*p3.
// The original keeps intermediates on the x87 stack (53-bit precision control); mirrored with double.
// Silence gate: common pattern, tail trunc(sr*0.5f); a skipped block also clears the three band envelopes.
// Oracle-verified (also with fx_compare --gated): bit-exact (-240 dB) over --random 8 and all 0 / all 1 / extreme sets.
#include "effect_base.h"

namespace hs2 {
namespace {

using X = double;  // the original runs the x87 at 53-bit precision (MSVC default control word)

class Multiband : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.5);  // +0xc tail: trunc(sr * 0.5)
        sr_ = sr;
        const X inv = X(1.f) / X(sr);
        invSr_ = float(inv);
        c2_ = float(X(250.f) * inv);
        c1_ = float(inv * X(18000.f));
        envH_ = envM_ = envL_ = 0.f;
        lp1L_ = lp1R_ = lp2L_ = lp2R_ = 0.f;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // Silence gate: the common pattern, but the skip branch (silent, countdown < 0) also clears the three band envelopes (+0xa8..0xb0).
    void processGated(float* lr, int n, bool& silent) override {
        if (silent && gateCount_ < 0) {
            envH_ = envM_ = envL_ = 0.f;
            return;
        }
        EffectBase::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        for (int i = 0; i < n; ++i) {
            const X xl = io[2 * i], xr = io[2 * i + 1];
            const X l1 = ((xl + X(1e-8f)) - X(lp1L_)) * X(c1_) + X(lp1L_);
            lp1L_ = float(l1);
            lp1R_ = float(((xr + X(1e-8f)) - X(lp1R_)) * X(c1_) + X(lp1R_));
            lp2L_ = float((l1 - X(lp2L_)) * X(c2_) + X(lp2L_));
            lp2R_ = float((X(lp1R_) - X(lp2R_)) * X(c2_) + X(lp2R_));
            const float hiL = float(xl - l1);
            const X hiR = xr - X(lp1R_);
            const X gH = X(gHi_) / (follow(envH_, X(hiL) + hiR) * X(amount_) + X(1.f));
            const float accL1 = float(X(hiL) * gH);
            const X accR1 = hiR * gH;
            const float midL = float(X(lp1L_) - X(lp2L_));
            const X midR = X(lp1R_) - X(lp2R_);
            const X gM = X(gMid_) / (follow(envM_, X(midL) + midR) * X(amount_) + X(1.f));
            const float accL = float(X(midL) * gM + X(accL1));
            const float accR = float(gM * midR + accR1);
            const X gL = X(gLo_) / (follow(envL_, X(lp2L_) + X(lp2R_)) * X(amount_) + X(1.f));
            io[2 * i] = float(gL * X(lp2L_) + X(accL));
            io[2 * i + 1] = float(gL * X(lp2R_) + X(accR));
        }
    }

private:
    X follow(float& env, X s) const {
        const X a = std::fabs(s);
        const float c = a > X(env) ? atk_ : rel_;
        const X e = (a - X(env)) * X(c) + X(env);
        env = float(e);
        return e;
    }
    static float coef(X a, X b, X p, float invSr) {
        return float(X(1.f) - std::exp(-(std::exp(a - p * b) * X(invSr))));
    }
    void onParam(int i, float v) override {
        const X p = v;
        switch (i) {
            case 0: case 2: case 3: case 4: {
                const X q = p_[2];
                amount_ = float(q * q * q * X(30.f));
                X e = std::exp((X(p_[0]) * X(4.6f) + q * X(2.4f)) - X(2.3f));
                e = e + e;
                const X h = e * X(p_[3]);
                gHi_ = float(h);
                gMid_ = float(e * X(p_[4]) * X(0.5f));
                gLo_ = float(e - h);
                return;
            }
            case 5: atk_ = coef(X(9.f), X(4.f), p, invSr_); return;
            case 6: rel_ = coef(X(3.6f), X(2.6f), p, invSr_); return;
            default: return;
        }
    }
    float c1_ = 0, c2_ = 0, amount_ = 0, gHi_ = 0, gMid_ = 0, gLo_ = 0, atk_ = 0, rel_ = 0;
    float envH_ = 0, envM_ = 0, envL_ = 0, lp1L_ = 0, lp1R_ = 0, lp2L_ = 0, lp2R_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_92() { return std::make_unique<Multiband>(); }

}  // namespace hs2
