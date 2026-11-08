// 6 Early Reflections (ctor FUN_10028760, setSampleRate FUN_100287a0, process FUN_10028850, setParam FUN_10028b50;
// vtable 0x100a8bd0, init/tempo 0x10002170 unused).
// Mono sum -> lowpass (p4) -> low cut (p3) -> allpass network in one 0.2 s line: two stages with gain g*0.83333,
// one with gain g, then two parallel stages (gain g) fed by the same third-stage value: their outputs are R and L.
// Base delay D = floor(p2^2 * sr * 0.025) + 10, taps D * {0.3747, 0.2823, 0.7309, 1, 0.913}; g = 0.9 - 0.4*p2.
// p0 mix (linear crossfade). The x87 code keeps several values in 53-bit registers; mirrored with doubles.
// Oracle-verified: bit-exact (-240 dB floor) over --random 8 plus all-0 / all-1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class EarlyReflections : public EffectBase {
public:
    EarlyReflections() {
        p_[3] = 0.5f;  // in_ECX[0x15]
        p_[4] = 0.75f;  // in_ECX[0x16]
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        w_ = 0;
        lp_ = hp_ = 0;
        L_ = int(double(sr) * double(0.2f));
        buf_.assign(L_, 0.f);
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const int L = L_, L1 = L - 1;
        const double D = D_;
        const double g = g_;
        const double g5 = float(double(g_) * double(0.83333f));
        auto wrap = [&](int x) { return L1 < x ? x - L : x; };
        int t0 = wrap(w_ - int(D * double(-0.3747f)));
        int t1 = wrap(t0 - int(D * double(-0.2823f)));
        int t2 = wrap(t1 - int(D * double(-0.7309f)));
        int t3 = wrap(int(D) + t2);
        int t4 = wrap(t3 - int(D * double(-0.913f)));
        int w = w_;
        const double mix = mix_, lpc = lpCoef_, hpc = hpCoef_;
        float* B = buf_.data();
        for (int i = 0; i < n; ++i) {
            const double inL = io[2 * i], inR = io[2 * i + 1];
            --w;
            const double lp = ((inL + inR) - lp_) * lpc + lp_;
            lp_ = float(lp);
            const double x = lp - hp_;
            hp_ = float(x * hpc + hp_);
            const float af = float(B[t0] - g5 * x);
            const double ga = g5 * af;
            B[w + 1] = float(ga + x);
            if (w < 0) w = L1;
            --t0;
            const double b = double(B[t1]) - float(ga);
            B[t0 + 1] = float(g5 * b + af);
            if (t0 < 0) t0 = L1;
            --t1;
            const float cf = float(B[t2] - g * b);
            const float gc = float(g * cf);
            B[t1 + 1] = float(g * cf + b);
            if (t1 < 0) t1 = L1;
            --t2;
            const double e = double(B[t3]) - gc;
            B[t2 + 1] = float(g * e + cf);
            if (t2 < 0) t2 = L1;
            const double h = double(B[t4]) - gc;
            B[t3] = float(g * h + cf);
            if (--t3 < 0) t3 = L1;
            if (--t4 < 0) t4 = L1;
            io[2 * i] = float((h - inL) * mix + inL);
            io[2 * i + 1] = float((e - inR) * mix + inR);
        }
        w_ = w;
    }

private:
    void onParam(int i, float v) override {
        const double dv = v;
        switch (i) {
            case 0: mix_ = v; break;
            case 2:
                D_ = float(std::floor(dv * double(sr_) * dv * double(0.025f)) + 10.0);
                g_ = float(double(0.9f) - dv * double(0.4f));
                break;
            case 3: hpCoef_ = float(dv * double(invSr_) * 2000.0); break;
            case 4: lpCoef_ = float(1.0 - std::exp(-((dv * dv * dv * 90000.0 + 10000.0) * double(invSr_)))); break;
            default: break;
        }
    }
    int L_ = 0, w_ = 0;
    std::vector<float> buf_;
    float mix_ = 0.5f, D_ = 10, g_ = 0.7f, hpCoef_ = 0, lpCoef_ = 0, lp_ = 0, hp_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_6() { return std::make_unique<EarlyReflections>(); }

}  // namespace hs2
