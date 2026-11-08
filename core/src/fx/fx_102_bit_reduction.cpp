// 102 Bit Reduction (ctor FUN_10041040, setSampleRate FUN_10041060, setParam FUN_100412c0, process FUN_100410e0).
// Sample-and-hold every N samples (N = trunc(sr/(p2^3*48000+1000))+1); at each hold the input is quantised by
// masking float mantissa bits (p5 <= 0.5: linear, the input is offset by 12.5 so the exponent is fixed;
// p5 > 0.5: raw float masking = logarithmic), and the output then slews towards it linearly: per hold the step is
// (q - y)*1.5/N clamped to +-slew (p4: (p4^3*19800+200)/sr, or 10 above 0.95), steps below 1e-8 are dropped.
// p0 mix, p2 rate, p3 bits (0.1 or less: none removed), p4 slew rate, p5 mode.
// Oracle-verified: bit-exact (-240 dB) over --random 8 + extremes.
#include "effect_base.h"

namespace hs2 {
namespace {

inline unsigned fbits(float f) {
    unsigned u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float bitsf(unsigned u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

class BitReduction : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        yL_ = yR_ = stepL_ = stepR_ = 0;
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (mix_ == 0.f) return;
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            if (--count_ == 0) {
                count_ = hold_;
                stepL_ = step(inL, yL_);
                stepR_ = step(inR, yR_);
            }
            const double sL = double(stepL_) + yL_;
            yL_ = float(sL);
            yR_ = float(double(yR_) + stepR_);
            io[2 * i] = float((sL - inL) * mix_ + inL);
            io[2 * i + 1] = float((double(yR_) - inR) * mix_ + inR);
        }
    }

private:
    float step(float x, float y) const {
        const float q = bitsf(fbits(float(double(x) + add_)) & mask_);
        const double d = ((double(q) - sub_) - y) * scale_;
        float s = float(d);
        if (d > double(slew_)) s = slew_;
        else if (s < -slew_) s = -slew_;
        if (std::fabs(s) < 1e-08f) s = 0.f;
        return s;
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: {
                const double p3 = double(v) * v * v;
                hold_ = int(double(sr_) / (p3 * double(48000.f) + double(1000.f))) + 1;
                scale_ = float(1.5 / double(hold_));
                return;
            }
            case 4:
                if (v <= 0.95f) slew_ = float((double(v) * v * v * double(19800.f) + 200.0) * invSr_);
                else slew_ = 10.f;
                return;
            case 3: case 5: {
                unsigned sh = 0;
                const bool lin = p_[5] <= 0.5f;
                if (p_[3] > 0.1f) sh = unsigned((lin ? 6 : 14) - int(double(p_[3]) * double(-10.9f)));
                sh &= 31;
                mask_ = (0xffffffffu >> sh) << sh;
                add_ = lin ? bitsf(0x41480069) : 0.f;
                sub_ = lin ? bitsf(0x41480000) : 0.f;
                return;
            }
            default: return;
        }
    }
    int count_ = 1, hold_ = 1;
    unsigned mask_ = 0xffffffffu;
    float mix_ = 0.5f, add_ = 0, sub_ = 0, scale_ = 0, slew_ = 0;
    float yL_ = 0, yR_ = 0, stepL_ = 0, stepR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_102() { return std::make_unique<BitReduction>(); }

}  // namespace hs2
