// 100 Distortion (ctor FUN_1003f330, setSampleRate FUN_1003f360, setParam FUN_1003f980, process FUN_1003f3d0).
// Per channel: x' = x + 1e-8; one-pole tone filter lp += (x' - lp)*k; v = clamp(drive*src, -1, 1);
// y = (v - v^3)*bias + v^3; out = y*outGain, mixed with x' by p0.
// Tone p5: > 0.51 highpass (src = lp - x', k = 1-exp(-((p-0.51)^3*50000+50)/sr)); < 0.49 lowpass
// (src = lp, k = 1-exp(-(p^3*300000+500)/sr)); in between k = 1 (no filter, src = x').
// p0 mix, p2 drive exp(6.9p), p3 bias 4p-1, p4 out p^2/2, p5 tone. All intermediates stay in x87 registers
// (double here); the filter state and x' are spilled as float, but the filter input is the unrounded x'.
// Oracle-verified: bit-exact (-240 dB) over --random 8 + extremes.
#include "effect_base.h"

namespace hs2 {
namespace {

// FUN_1000c5b0: exp() of a float argument
inline double expf_arg(float a) { return std::exp(double(a)); }

class Distortion : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        lp_[0] = lp_[1] = 0;
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (mix_ == 0.f) return;
        for (int i = 0; i < 2 * n; ++i) {
            float& s = lp_[i & 1];
            const double xd = double(io[i]) + double(1e-08f);  // the filter uses the register value
            const float x = float(xd);                          // the hp reference and dry mix the spill
            const double l = (xd - s) * k_ + s;
            s = float(l);
            double v = (l - (hp_ ? double(x) : 0.0)) * drive_;
            if (v > 1.0) v = 1.0;
            else if (v < -1.0) v = -1.0;
            const double c = v * v * v;
            io[i] = float((((v - c) * bias_ + c) * out_ - x) * mix_ + x);
        }
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: drive_ = float(std::exp(double(v) * double(6.9f))); return;
            case 3: bias_ = float(double(v) * 4.0 - 1.0); return;
            case 4: out_ = float(double(v) * v * 0.5); return;
            case 5:
                if (v > 0.51f) {
                    const double d = double(v) - double(0.51f);
                    hp_ = true;
                    k_ = float(1.0 - std::exp(-((d * d * d * 50000.0 + 50.0) * invSr_)));
                    return;
                }
                k_ = 1.f;
                if (v < 0.49f) {
                    const float a = float(-((double(v) * v * v * 300000.0 + 500.0) * invSr_));
                    k_ = float(1.0 - expf_arg(a));
                }
                hp_ = false;
                return;
            default: return;
        }
    }
    bool hp_ = false;
    float mix_ = 0.5f, drive_ = 1, bias_ = 0, out_ = 0, k_ = 1;
    float lp_[2] = {0, 0};
};

}  // namespace

std::unique_ptr<Effect> make_fx_100() { return std::make_unique<Distortion>(); }

}  // namespace hs2
