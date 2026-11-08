// 8 Overheads (ctor FUN_10028d60, setSampleRate FUN_10028da0, process FUN_10028e60, setParam FUN_100292c0;
// vtable 0x100a8bfc, init/tempo 0x10002170 unused).
// Mono (L+R)/2 -> lowpass (p4) -> low cut (p3) -> two 4-stage allpass chains in one 0.5 s line, both fed by the
// filtered input, all with gain g = 0.3*(1+p5). Base delay D = floor(p2^2 * sr * 0.025) + 10, successive taps
// scaled by s = 0.75 + 0.4*p6 (with -0.03 / *0.807 / -0.07 tweaks). P = chain-1 out - chain-1 stage 3,
// Q = chain-2 stage 1 + chain-2 out; L = P + (P-Q)/2, R = Q - (P-Q)/2; p0 mix.
// The x87 code keeps several values in 53-bit registers; mirrored with double intermediates.
// Oracle-verified: bit-exact (-240 dB floor) over --random 8 plus all-0 / all-1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class Overheads : public EffectBase {
public:
    Overheads() {
        p_[3] = 0.5f;  // in_ECX[0x15]
        p_[4] = 0.75f;  // in_ECX[0x16]
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        w_ = 0;
        lp_ = hp_ = 0;
        L_ = int(double(sr) * 0.5);
        buf_.assign(L_, 0.f);
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const int L = L_, L1 = L - 1;
        auto wrap = [&](int x) { return L1 < x ? x - L : x; };
        const double s = s_;
        int t[8];
        double f = D_;
        t[0] = wrap(int(f) + w_);
        f *= s;
        t[1] = wrap(int(f) + t[0]);
        f *= s - double(0.03f);
        t[2] = wrap(int(f) + t[1]);
        f *= s;
        t[3] = wrap(int(f) + t[2]);
        f = f * s * double(0.807f);
        t[4] = wrap(int(f) + t[3]);
        f *= s;
        t[5] = wrap(int(f) + t[4]);
        f *= s - double(0.07f);
        t[6] = wrap(int(f) + t[5]);
        t[7] = wrap(int(s * f) + t[6]);
        int w = w_;
        const double g = g_, mix = mix_, lpc = lpCoef_, hpc = hpCoef_;
        float* B = buf_.data();
        auto dec = [&](int& x) {
            if (--x < 0) x = L1;
        };
        for (int i = 0; i < n; ++i) {
            const double inL = io[2 * i], inR = io[2 * i + 1];
            const double lp = ((inR + inL) * 0.5 - lp_) * lpc + lp_;
            lp_ = float(lp);
            const double x = lp - hp_;
            hp_ = float(x * hpc + hp_);
            const float xf = float(x);
            // chain 1
            const float af = float(B[t[0]] - g * x);
            --w;
            const float gaf = float(g * af);
            B[w + 1] = float(g * af + x);
            if (w < 0) w = L1;
            const double b = double(B[t[1]]) - gaf;
            const float bf = float(b);
            B[t[0]] = float(double(af) + g * b);
            dec(t[0]);
            const double c = B[t[2]] - g * b;
            B[t[1]] = float(double(bf) + g * c);
            dec(t[1]);
            const float df = float(B[t[3]] - g * c);
            B[t[2]] = float(g * df + c);
            dec(t[2]);
            // chain 2 (input again)
            const float ef = float(B[t[4]] - g * xf);
            B[t[3]] = float(double(xf) + g * ef);
            dec(t[3]);
            const float hf = float(B[t[5]] - g * ef);
            B[t[4]] = float(double(ef) + g * hf);
            dec(t[4]);
            const float kf = float(B[t[6]] - g * hf);
            B[t[5]] = float(double(hf) + g * kf);
            dec(t[5]);
            const double m = B[t[7]] - g * kf;
            B[t[6]] = float(g * m + kf);
            dec(t[6]);
            dec(t[7]);
            const double P = m - c;
            const double Q = double(df) + kf;
            const double H = (P - Q) * 0.5;
            io[2 * i] = float(((P + H) - inL) * mix + inL);
            io[2 * i + 1] = float(((Q - H) - inR) * mix + inR);
        }
        w_ = w;
    }

private:
    void onParam(int i, float v) override {
        const double dv = v;
        switch (i) {
            case 0: mix_ = v; break;
            case 2: D_ = float(std::floor(dv * double(sr_) * dv * double(0.025f)) + 10.0); break;
            case 3: hpCoef_ = float(dv * double(invSr_) * 4000.0); break;
            case 4: lpCoef_ = float(1.0 - std::exp(-((dv * dv * dv * 90000.0 + 10000.0) * double(invSr_)))); break;
            case 5: g_ = float((dv + 1.0) * double(0.3f)); break;
            case 6: s_ = float(dv * double(0.4f) + 0.75); break;
            default: break;
        }
    }
    int L_ = 0, w_ = 0;
    std::vector<float> buf_;
    float mix_ = 0.5f, D_ = 10, g_ = 0.3f, s_ = 0.75f, hpCoef_ = 0, lpCoef_ = 0, lp_ = 0, hp_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_8() { return std::make_unique<Overheads>(); }

}  // namespace hs2
