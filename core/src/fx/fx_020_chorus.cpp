// 20 Chorus (ctor FUN_10031000, setSampleRate FUN_10031020, setParam FUN_10031440, process FUN_10031100).
// Mono-sum 50 ms line; L/R fractional taps driven by sin/cos LFOs (R at 1.41x). Every 256 samples each tap gets
// a read velocity that glides it to the LFO target. p0 mix, p1/p2 rate (last set wins), p3 depth, p4 base delay.
// x87 intermediates mirrored with double (53-bit precision control); float only where the original stores.
// Oracle-verified: bit-exact.
#include "effect_base.h"

namespace hs2 {
namespace {

// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale (53-bit precision control).
double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}

class Chorus : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.05f));
        buf_.assign(size_t(L_) + 2, 0.f);
        w_ = iL_ = iR_ = 0;
        count_ = 1;
        phL_ = phR_ = 0;
        fracL_ = fracR_ = velL_ = velR_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const double m = mix_, m2 = m * m;
        const float dry = float(1.0 - m2), wet = float(((m + m) - m2) * 0.5);
        const int L1 = L_ - 1;
        constexpr double kTwoPi = double(6.2831855f);
        const float incR = inc_ * 1.41f;  // stored once per call
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                const double sl = double(phL_) + inc_;  // wrap test and subtraction on the unrounded sum
                phL_ = float(kTwoPi < sl ? sl - kTwoPi : sl);
                int d = iL_ - w_;
                if (d < 0) d += L_;
                velL_ = float(((((double(std::sin(double(phL_))) + 1.0) * depth_ + base_) - fracL_) - d) *
                              0.00390625 - 1.0);
                if (velL_ > 0.f) velL_ = 0.f;
                const double sr = double(incR) + phR_;
                phR_ = float(kTwoPi < sr ? sr - kTwoPi : sr);
                d = iR_ - w_;
                if (d < 0) d += L_;
                velR_ = float(((((double(std::cos(double(phR_))) + 1.0) * depth_ + base_) - fracR_) - d) *
                              0.00390625 - 1.0);
                if (velR_ > 0.f) velR_ = 0.f;
                count_ = 256;
            }
            const float inL = io[2 * i], inR = io[2 * i + 1];
            float a = buf_[iL_];
            io[2 * i] = float(((double(buf_[iL_ + 1]) - a) * fracL_ + a) * wet + double(inL) * dry);
            fracL_ += velL_;
            while (fracL_ < 0.f) {
                --iL_;
                fracL_ += 1.f;
                if (iL_ < 0) iL_ = L1;
            }
            a = buf_[iR_];
            io[2 * i + 1] = float(((double(buf_[iR_ + 1]) - a) * fracR_ + a) * wet + double(inR) * dry);
            fracR_ += velR_;
            while (fracR_ < 0.f) {
                --iR_;
                fracR_ += 1.f;
                if (iR_ < 0) iR_ = L1;
            }
            --w_;
            buf_[w_ + 1] = inR + inL;
            if (w_ < 0) {
                buf_[L_] = buf_[0];
                w_ = L1;
            }
        }
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2:
            {  // the unrounded exp result scales the increment; the float copy (this+0x90) is used for depth
                const double r = x87exp(double(v) * 5.991464138031006 - 2.995732069015503);
                rate_ = float(r);
                inc_ = float(r * invSr_ * 677.298828125);
            }
                [[fallthrough]];
            case 3: {
                double f = double(0.02f) / rate_;
                if (!(f <= double(0.024f))) f = double(0.024f);
                depth_ = float(f * sr_ * p_[3] * p_[3] * 0.5);
                return;
            }
            case 4: base_ = float(double(v) * sr_ * double(0.024f) + 1.0); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, iL_ = 0, iR_ = 0, count_ = 1;
    float mix_ = 0.5f, rate_ = 1.f, inc_ = 0, depth_ = 0, base_ = 1;
    float phL_ = 0, phR_ = 0, fracL_ = 0, fracR_ = 0, velL_ = 0, velR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_20() { return std::make_unique<Chorus>(); }

}  // namespace hs2
