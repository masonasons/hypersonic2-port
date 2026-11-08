// 0 Reverb (ctor FUN_100257c0, setSampleRate FUN_10025820, process FUN_10025a50, setParam FUN_10025f50;
// vtable 0x100a8a5c, init/tempo 0x10002170 unused).
// Mono sum -> lowpass (p4) -> low cut (p3) -> 4-stage allpass chain (gain 0.6, lengths sr*0.05209 * 0.787^k)
// -> 4 combs (lengths sr*0.15331 * 0.79^k, feedback p2: 0.66 - (1-p2)^2/2) cross-fed through a Hadamard-like
// matrix of the previous comb states. Output: dry*(1-m^2) +/- wet*(m/4)/(fb+0.5) of two comb sums (p0 = m).
// The x87 code keeps several values in 53-bit registers; mirrored with double intermediates.
// Oracle-verified: bit-exact (-240 dB floor) over --random 8 plus all-0 / all-1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class Reverb : public EffectBase {
public:
    Reverb() {
        p_[3] = 0.5f;  // in_ECX[0x15]
        p_[4] = 0.75f;  // in_ECX[0x16]
    }
    void setSampleRate(float sr) override {
        gate_ = Gate::Detect;  // keeps running while its output is audible (FUN_10025480)
        sr_ = sr;
        invSr_ = 1.f / sr;
        La_ = int(double(sr) * double(0.5f));
        Lc_ = int(double(sr) * double(0.2f));
        ap_.assign(La_, 0.f);
        for (auto& c : comb_) c.assign(Lc_, 0.f);
        wa_ = wc_ = 0;
        lp_ = hp_ = 0;
        for (auto& e : e_) e = 0;
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (La_ <= 0 || Lc_ <= 0) return;
        const double m = mix_;
        const double dry = float(1.0 - m * m);
        const double fb = fb_;
        const double wet = float((m * double(0.25f)) / (fb + 0.5));
        const int La = La_, La1 = La - 1, Lc = Lc_, Lc1 = Lc - 1;
        // allpass taps
        double f = apBase_;
        int t0 = int(f) + wa_;
        if (La1 < t0) t0 -= La;
        f *= double(0.787f);
        int t1 = int(f) + t0;
        if (La1 < t1) t1 -= La;
        f *= double(0.787f);
        int t2 = int(f) + t1;
        if (La1 < t2) t2 -= La;
        int t3 = t2 - int(f * double(-0.787f));
        if (La1 < t3) t3 -= La;
        // comb taps
        f = combBase_;
        int c[4];
        c[0] = int(f) + wc_;
        f *= double(0.79f);
        c[1] = int(f) + wc_;
        f *= double(0.79f);
        c[2] = int(f) + wc_;
        c[3] = wc_ - int(f * double(-0.79f));
        for (int& x : c)
            if (Lc1 < x) x -= Lc;
        int wa = wa_, wc = wc_;
        const double g = double(0.6f);
        const double lpc = lpCoef_, hpc = hpCoef_;
        for (int i = 0; i < n; ++i) {
            const double inL = io[2 * i], inR = io[2 * i + 1];
            --wa;
            const double lp = (((inR + inL) + double(9.99999993922529e-09f)) - lp_) * lpc + lp_;
            lp_ = float(lp);
            const double x = lp - hp_;
            hp_ = float(x * hpc + hp_);
            // allpass chain
            const double a = ap_[t0] - g * x;
            const float af = float(a);
            const float ga = float(g * a);
            ap_[wa + 1] = float(g * a + x);
            if (wa < 0) wa = La1;
            --t0;
            const double b = double(ap_[t1]) - ga;
            const float bf = float(b);
            const double gb = g * b;
            ap_[t0 + 1] = float(double(af) + gb);
            if (t0 < 0) t0 = La1;
            --t1;
            const float cf = float(ap_[t2] - gb);
            const double gc = g * cf;
            ap_[t1 + 1] = float(double(bf) + gc);
            if (t1 < 0) t1 = La1;
            const double d = ap_[t3] - gc;
            ap_[t2] = float(g * d + cf);
            if (--t2 < 0) t2 = La1;
            if (--t3 < 0) t3 = La1;
            if (--wc < 0) wc = Lc1;
            for (int& x2 : c)
                if (--x2 < 0) x2 = Lc1;
            // combs
            const double e0 = comb_[0][c[0]] * fb + d;
            e_[0] = float(e0);
            comb_[0][wc] = float(double(e_[1]) + e_[2]);
            const double e1 = comb_[1][c[1]] * fb + d;
            e_[1] = float(e1);
            comb_[1][wc] = float(double(e_[0]) + e_[3]);
            const double r2 = comb_[2][c[2]];
            const float s1 = float(r2 + e1);
            e_[2] = float(r2 * fb + d);
            comb_[2][wc] = float(double(e_[0]) - e_[3]);
            const double r3 = comb_[3][c[3]];
            e_[3] = float(r3 * fb + d);
            comb_[3][wc] = float(double(e_[1]) - e_[2]);
            io[2 * i] = float(s1 * wet + inL * dry);
            io[2 * i + 1] = float(inR * dry - (r3 + e0) * wet);
        }
        wa_ = wa;
        wc_ = wc;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; break;
            case 2: {
                const double u = 1.0 - double(v);
                fb_ = float(double(0.66f) - u * u * 0.5);
                break;
            }
            case 3: hpCoef_ = float(double(v) * double(invSr_) * 2000.0); break;
            case 4: {
                const double dv = v;
                lpCoef_ = float(1.0 - std::exp(-((dv * dv * dv * 90000.0 + 10000.0) * double(invSr_))));
                break;
            }
            default: break;
        }
        apBase_ = float(std::floor(double(sr_) * double(0.05209f)));
        combBase_ = float(std::floor(double(sr_) * double(0.15331f)));
    }
    int La_ = 0, Lc_ = 0, wa_ = 0, wc_ = 0;
    std::vector<float> ap_, comb_[4];
    float mix_ = 0.5f, fb_ = 0, hpCoef_ = 0, lpCoef_ = 0, apBase_ = 0, combBase_ = 0;
    float lp_ = 0, hp_ = 0, e_[4] = {};
};

}  // namespace

std::unique_ptr<Effect> make_fx_0() { return std::make_unique<Reverb>(); }

}  // namespace hs2
