// 81 Parametric EQ (ctor FUN_1003b9e0, setSampleRate FUN_1003ba00, setParam FUN_1003c5f0, coefficients FUN_1003bad0,
// process FUN_1003bec0). Two SVF peaking bands: y = x + g*bp, output gain p0 (+-20 dB).
// p2/p5 gain, p3/p6 freq (31 Hz..16 kHz), p4/p7 Q. The SVF f coefficient comes from a 9x9 table (DLL @0x100a88d8)
// with bilinear interpolation over freq/sr*20 and 3*Q. Precision mirrored from the disassembly (53-bit x87).
// Oracle-verified: see docs/NOTES.md.
#include "effect_base.h"

namespace hs2 {
namespace {

// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale (53-bit precision control).
double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}

class ParametricEQ : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.5);  // +0xc tail: trunc(sr * 0.5)
        sr_ = sr;
        invSr_ = 1.f / sr;
        changes_ = 1;
        seen_ = 0;
        for (auto& b : st_) b = Band{};
        g1cur_ = g2cur_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (seen_ != changes_) recalc();
        if (mode_ == 1) {
            smooth(g1cur_, g1_);
            g2cur_ = g2_;
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c) {
                    const double x = double(io[2 * i + c]) + double(1e-8f);
                    const float bp = svf(st_[c], x, f1_, q1_);
                    io[2 * i + c] = float((double(g1cur_) * bp + x) * outGain_);
                }
        } else if (mode_ == 2) {
            smooth(g1cur_, g1_);
            smooth(g2cur_, g2_);
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c) {
                    double x = double(io[2 * i + c]) + double(1e-8f);
                    x = double(g1cur_) * svf(st_[c], x, f1_, q1_) + x;
                    io[2 * i + c] = float((double(svf(st_[2 + c], x, f2_, q2_)) * g2cur_ + x) * outGain_);
                }
        }
    }

private:
    struct Band {
        float bp = 0, lp = 0, xprev = 0;
    };
    // x87: the new bp is stored as float but its register value updates lp; the output reloads the float bp.
    static float svf(Band& s, double x, float f, float q) {
        const double bp = s.bp - ((double(s.bp) + s.xprev + x) * q + s.lp) * f;
        s.bp = float(bp);
        s.lp = float(bp * f + s.lp);
        s.xprev = float(x);
        return s.bp;
    }
    static void smooth(float& cur, float target) {
        if (cur != target) {
            const double c = (double(target) - cur) * double(0.1f) + cur;
            cur = float(c);
            if (std::fabs(c - target) < double(0.001f)) cur = target;
        }
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: outGain_ = float(x87exp(double(v) * double(4.6052f) - double(2.3026f))); return;
            case 3: freq1_ = float(x87exp(double(v) * double(6.2383f) + double(3.442f))); break;
            case 6: freq2_ = float(x87exp(double(v) * double(6.2383f) + double(3.442f))); break;
            case 2: case 4: case 5: case 7: break;
            default: return;
        }
        ++changes_;
    }
    void band(float gainP, float freq, float qP, float& g, float& q, float& f) const {
        static const float kT[82] = {
            0.0000f, 0.3140f, 0.6120f, 0.9070f, 1.1690f, 1.4160f, 1.6210f, 1.7770f, 1.8980f,
            0.0000f, 0.3120f, 0.6090f, 0.8920f, 1.1510f, 1.3880f, 1.5830f, 1.7300f, 1.8490f,
            0.0000f, 0.3100f, 0.6030f, 0.8850f, 1.1320f, 1.3550f, 1.5450f, 1.6860f, 1.7950f,
            0.0000f, 0.3080f, 0.5910f, 0.8600f, 1.0920f, 1.3010f, 1.4720f, 1.5970f, 1.6910f,
            0.0000f, 0.3000f, 0.5680f, 0.8170f, 1.0160f, 1.1920f, 1.3270f, 1.4260f, 1.5080f,
            0.0000f, 0.2920f, 0.5270f, 0.7270f, 0.8780f, 1.0030f, 1.0950f, 1.1580f, 1.2010f,
            0.0000f, 0.2690f, 0.4550f, 0.5870f, 0.6720f, 0.7340f, 0.7720f, 0.8000f, 0.8100f,
            0.0000f, 0.2300f, 0.3430f, 0.4020f, 0.4330f, 0.4500f, 0.4590f, 0.4650f, 0.4600f,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            641.0981f};
        g = float((x87exp(double(gainP) * double(4.6052f) - double(2.3026f)) - 1.0) * -0.5);
        const double fiU = double(freq) * invSr_ * 20.0;
        float fi = float(fiU);
        if (fiU > 8.0) fi = 8.f;
        const int base = g >= 0.f ? 0x24 : 9;
        int idx = int(fi) + base;
        const float ffrac = float(std::fmod(double(fi), 1.0));
        q = float(x87exp(double(qP) * double(2.07944f) - double(g >= 0.f ? 0.69315f : 2.7726f)));
        const double q3U = double(qP) * 3.0;
        const float q3 = float(q3U);
        idx += int(q3U) * 9;
        const float a = float((double(kT[idx + 1]) - kT[idx]) * ffrac + kT[idx]);
        const double b = (double(kT[idx + 10]) - kT[idx + 9]) * ffrac + kT[idx + 9];
        f = float((b - a) * std::fmod(double(q3), 1.0) + a);
    }
    void recalc() {
        const int old = mode_;
        seen_ = changes_;
        mode_ = 0;
        band(p_[2], freq1_, p_[4], g1_, q1_, f1_);
        if (std::fabs(g1_) > 0.01f) mode_ = 1;
        band(p_[5], freq2_, p_[7], g2_, q2_, f2_);
        if (std::fabs(g2_) > 0.01f) {
            if (mode_ == 1) mode_ = 2;
            else {
                mode_ = 1;
                g1_ = g2_;
                q1_ = q2_;
                f1_ = f2_;
            }
        }
        if (mode_ == 0 && std::fabs(outGain_ - 1.f) > 0.01f) mode_ = 1;
        if (old != mode_) setSampleRate(sr_);
    }
    Band st_[4];
    int mode_ = 0, changes_ = 0, seen_ = 0;
    float outGain_ = 1, freq1_ = 0, freq2_ = 0;
    float g1_ = 0, q1_ = 0, f1_ = 0, g2_ = 0, q2_ = 0, f2_ = 0, g1cur_ = 0, g2cur_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_81() { return std::make_unique<ParametricEQ>(); }

}  // namespace hs2
