// 2 Plate Reverb (ctor FUN_10026eb0, setSampleRate FUN_10026f20, process FUN_10027250, setParam FUN_10027850;
// vtable 0x100a8b2c, init/tempo 0x10002170 unused).
// Mono predelay (p3, up to 0.4 s; 4*L line whose wrap subtracts 4L-1 like the original) -> lowpass (p6, input
// scaled 0.2) -> low cut (p5) -> 6 series allpasses (2 x 0.833, 4 x 0.65; taps from D*0.39) -> 4 combs (D*0.91^k,
// feedback p2: 0.66 - (1-p2)^3/2) cross-fed through a Hadamard-like matrix of the previous comb outputs.
// L/R = allpass out + two different comb sums; p0 mix. p4 computes a damping coefficient that process never
// uses (as in the original). The x87 code keeps several values in 53-bit registers; mirrored with doubles.
// Oracle-verified: bit-exact (-240 dB floor) over --random 8 plus all-0 / all-1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class PlateReverb : public EffectBase {
public:
    PlateReverb() {
        p_[3] = 0.25f;  // in_ECX[0x15]
        p_[4] = 0.75f;  // in_ECX[0x16]
        p_[6] = 0.25f;  // in_ECX[0x18]
    }
    void setSampleRate(float sr) override {
        gate_ = Gate::Detect;  // keeps running while its output is audible (FUN_10025480)
        sr_ = sr;
        invSr_ = 1.f / sr;
        preW_ = w_ = 0;
        hp_ = lp_ = 0;
        for (auto& e : e_) e = 0;
        L_ = int(double(sr) * double(0.1f));
        pre_.assign(size_t(L_) * 4, 0.f);
        ap_.assign(L_, 0.f);
        for (auto& c : comb_) c.assign(L_, 0.f);
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const int L = L_, L1 = L - 1;
        auto wrap = [&](int x) { return L1 < x ? x - L : x; };
        double f = double(D_) * double(0.39f);
        int a[6];
        a[0] = wrap(w_ - int(f * double(-0.3747f)));
        a[1] = wrap(a[0] - int(f * double(-0.2823f)));
        a[2] = wrap(a[1] - int(f * double(-0.7309f)));
        a[3] = wrap(a[2] - int(f * double(-0.15f)));
        a[4] = wrap(a[3] - int(f * double(-0.79f)));
        a[5] = wrap(int(f) + a[4]);
        f = D_;
        int c[4];
        c[0] = wrap(int(f) + w_);
        f *= double(0.91f);
        c[1] = wrap(int(f) + w_);
        f *= double(0.91f);
        c[2] = wrap(int(f) + w_);
        c[3] = wrap(w_ - int(f * double(-0.91f)));
        const int P1 = L * 4 - 1;
        int pw = preW_;
        int pr = predelay_ + pw;
        if (P1 < pr) pr -= P1;
        int w = w_;
        const double fb = fb_, mix = mix_, lpc = lpCoef_, hpc = hpCoef_;
        const double g8 = double(0.833f), g6 = double(0.65f);
        float* B = ap_.data();
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            --pw;
            pre_[pw + 1] = float((double(inR) + inL) + double(9.99999993922529e-09f));
            if (pw < 0) pw = P1;
            lp_ = float((double(pre_[pr]) * double(0.2f) - lp_) * lpc + lp_);
            if (--pr < 0) pr = P1;
            const double x = double(lp_) - hp_;
            hp_ = float(x * hpc + hp_);
            // allpasses
            const double v1 = B[a[0]] - g8 * x;
            --w;
            const float v1f = float(v1);
            const float gv1 = float(g8 * v1);
            B[w + 1] = float(g8 * v1 + x);
            if (w < 0) w = L1;
            const double v2 = double(B[a[1]]) - gv1;
            --a[0];
            const float v2f = float(v2);
            B[a[0] + 1] = float(g8 * v2 + v1f);
            if (a[0] < 0) a[0] = L1;
            const double v3 = B[a[2]] - g6 * v2f;
            --a[1];
            const float v3f = float(v3);
            const double gv3 = g6 * v3;
            B[a[1] + 1] = float(double(v2f) + gv3);
            if (a[1] < 0) a[1] = L1;
            const float v4f = float(B[a[3]] - gv3);
            --a[2];
            const double gv4 = g6 * v4f;
            B[a[2] + 1] = float(double(v3f) + gv4);
            if (a[2] < 0) a[2] = L1;
            const float v5f = float(B[a[4]] - gv4);
            --a[3];
            const double gv5 = g6 * v5f;
            B[a[3] + 1] = float(double(v4f) + gv5);
            if (a[3] < 0) a[3] = L1;
            const double v6 = B[a[5]] - gv5;
            B[a[4]] = float(g6 * v6 + v5f);
            if (--a[4] < 0) a[4] = L1;
            if (--a[5] < 0) a[5] = L1;
            // combs (write index shared with the allpass line)
            const int k = w;
            if (--c[0] < 0) c[0] = L1;
            const double e0 = comb_[0][c[0]] * fb + v6;
            e_[0] = float(e0);
            comb_[0][k] = float(double(e_[1]) + e_[2]);
            if (--c[1] < 0) c[1] = L1;
            const double e1 = comb_[1][c[1]] * fb + v6;
            e_[1] = float(e1);
            comb_[1][k] = float(double(e_[3]) + e_[0]);
            if (--c[2] < 0) c[2] = L1;
            const double r2 = comb_[2][c[2]];
            const float s = float(e1 + r2);
            e_[2] = float(r2 * fb + v6);
            comb_[2][k] = float(double(e_[0]) - e_[3]);
            if (--c[3] < 0) c[3] = L1;
            const double r3 = comb_[3][c[3]];
            e_[3] = float(r3 * fb + v6);
            comb_[3][k] = float(double(e_[1]) - e_[2]);
            io[2 * i] = float(((v6 + s) - inL) * mix + inL);
            io[2 * i + 1] = float((((e0 + r3) + v6) - inR) * mix + inR);
        }
        w_ = w;
        preW_ = pw;
    }

private:
    void onParam(int i, float v) override {
        const double dv = v;
        switch (i) {
            case 0: mix_ = v; break;
            case 2: {
                D_ = float(double(sr_) * double(0.09f));
                const double u = 1.0 - dv;
                fb_ = float(double(0.66f) - u * u * u * 0.5);
                break;
            }
            case 3: predelay_ = int(dv * double(sr_) * dv * double(0.399f)); break;
            case 5: hpCoef_ = float(dv * double(invSr_) * 2000.0); break;
            case 6: lpCoef_ = float(1.0 - std::exp(-((dv * dv * dv * 90000.0 + 10000.0) * double(invSr_)))); break;
            default: break;  // p4: damping coefficient stored at +0x8c but never read
        }
    }
    int L_ = 0, w_ = 0, preW_ = 0, predelay_ = 0;
    std::vector<float> pre_, ap_, comb_[4];
    float mix_ = 0.5f, D_ = 0, fb_ = 0, hpCoef_ = 0, lpCoef_ = 0;
    float lp_ = 0, hp_ = 0, e_[4] = {};
};

}  // namespace

std::unique_ptr<Effect> make_fx_2() { return std::make_unique<PlateReverb>(); }

}  // namespace hs2
