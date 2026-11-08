// 5 Non-Linear Reverb (ctor FUN_1002a970, setSampleRate FUN_1002a9d0, process FUN_1002ac50, setParam FUN_1002b0c0;
// vtable 0x100a8d08, init/tempo 0x10002170 unused).
// Mono sum -> lowpass (p4) -> low cut (p3) -> 4-stage allpass (gain 0.5, lengths base*0.79^k, base from p2)
// -> five 1 s tapped lines in a feed-forward cascade (no feedback: a gated / "non-linear" tail) whose tap spacing
// is n*{2520,504,360,315,280} samples, n from p2 (even-ised above 5, then halved). p0 mix (dry 1-m^2, wet m^2/5).
// Any p2 change makes the next process() call re-run setSampleRate (clears all lines), like the original.
// The x87 code keeps several values in 53-bit registers; mirrored with double intermediates.
// Oracle-verified: bit-exact (-240 dB floor) over --random 8 plus all-0 / all-1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class NonLinearReverb : public EffectBase {
public:
    NonLinearReverb() {
        p_[3] = 0.5f;  // in_ECX[0x15]
        p_[4] = 0.75f;  // in_ECX[0x16]
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        wc_ = wa_ = 0;
        lp_ = hp_ = 0;
        La_ = int(double(sr) * double(0.5f));
        ap_.assign(La_, 0.f);
        Lc_ = int(sr);
        for (auto& b : line_) b.assign(Lc_, 0.f);
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (reset_) {
            setSampleRate(sr_);
            reset_ = false;
        }
        if (La_ <= 0 || Lc_ <= 0) return;
        const double m = mix_;
        const double m2 = m * m;
        const double dry = float(1.0 - m2);
        const double wet = float(m2 * double(0.2f));
        const int La = La_, La1 = La - 1, Lc = Lc_, Lc1 = Lc - 1;
        double f = apBase_;
        int t0 = int(f) + wa_;
        if (La1 < t0) t0 -= La;
        f *= double(0.79f);
        int t1 = int(f) + t0;
        if (La1 < t1) t1 -= La;
        f *= double(0.79f);
        int t2 = int(f) + t1;
        if (La1 < t2) t2 -= La;
        int t3 = t2 - int(f * double(-0.79f));
        if (La1 < t3) t3 -= La;
        int k = mult_;
        if (5 < k) k &= 0xfffe;
        int c[5];
        c[0] = wc_ + k * 2520;
        if (5 < k) k >>= 1;
        c[1] = wc_ + k * 504;
        c[2] = wc_ + k * 360;
        c[3] = wc_ + k * 315;
        c[4] = wc_ + k * 280;
        for (int& x : c)
            if (Lc1 < x) x -= Lc;
        int wa = wa_, wc = wc_;
        const double lpc = lpCoef_, hpc = hpCoef_;
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            const double lp = (((double(inL) + inR) + double(9.99999993922529e-09f)) - lp_) * lpc + lp_;
            lp_ = float(lp);
            const double x = lp - hp_;
            hp_ = float(x * hpc + hp_);
            // allpass chain
            const double a = ap_[t0] - 0.5 * x;
            --wa;
            const float af = float(a);
            const float ha = float(0.5 * a);
            ap_[wa + 1] = float(0.5 * a + x);
            if (wa < 0) wa = La1;
            const double b = double(ap_[t1]) - ha;
            --t0;
            const float bf = float(b);
            const double hb = 0.5 * b;
            ap_[t0 + 1] = float(double(af) + hb);
            if (t0 < 0) t0 = La1;
            const float cf = float(ap_[t2] - hb);
            --t1;
            const double hc = 0.5 * cf;
            ap_[t1 + 1] = float(double(bf) + hc);
            if (t1 < 0) t1 = La1;
            const double d = ap_[t3] - hc;
            ap_[t2] = float(0.5 * d + cf);
            if (--t2 < 0) t2 = La1;
            if (--t3 < 0) t3 = La1;
            if (--wc < 0) wc = Lc1;
            // tapped-line cascade
            if (--c[0] < 0) c[0] = Lc1;
            const float y = float(d - line_[0][c[0]]);
            line_[0][wc] = float(d);
            if (--c[1] < 0) c[1] = Lc1;
            const double s = d + line_[1][c[1]];
            line_[1][wc] = float(double(y) + line_[1][c[1]]);
            if (--c[2] < 0) c[2] = Lc1;
            const float z = float(s - line_[2][c[2]]);
            line_[2][wc] = float(double(y) + line_[2][c[2]]);
            if (--c[3] < 0) c[3] = Lc1;
            const double sR = double(z) + line_[3][c[3]];
            line_[3][wc] = float(double(y) + line_[3][c[3]]);
            if (--c[4] < 0) c[4] = Lc1;
            const double sL = double(z) + line_[4][c[4]];
            line_[4][wc] = float(double(y) + line_[4][c[4]]);
            io[2 * i] = float(sL * wet + double(inL) * dry);
            io[2 * i + 1] = float(sR * wet + double(inR) * dry);
        }
        wa_ = wa;
        wc_ = wc;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; break;
            case 2: {
                const double dv = v;
                mult_ = int((dv * double(0.45f) + double(0.05f)) * double(sr_) * double(0.00039682540f));
                if (mult_ == 0) mult_ = 1;
                apBase_ = float(std::floor((dv * double(0.44f) + double(0.1f)) * double(sr_) * double(0.05209f)));
                reset_ = true;
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
    }
    int La_ = 0, Lc_ = 0, wa_ = 0, wc_ = 0, mult_ = 0;
    bool reset_ = false;
    std::vector<float> ap_, line_[5];
    float mix_ = 0.5f, hpCoef_ = 0, lpCoef_ = 0, apBase_ = 0;
    float lp_ = 0, hp_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_5() { return std::make_unique<NonLinearReverb>(); }

}  // namespace hs2
