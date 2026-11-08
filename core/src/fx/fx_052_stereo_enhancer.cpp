// 52 Stereo Enhancer (ctor FUN_10034af0, vtable 0x100a9308: setSampleRate FUN_10034b10, process FUN_10034bc0,
// setParam FUN_10035950; init/tempo 0x10002170 is a no-op).
// Per channel a cascade of 1..3 first-order allpasses y = (y' - x) a + x' (stage count 1 - trunc(-2.9 p5)).
// Stage k corner f = 100 e^(4.6052 p(2+k)) Hz (as f/sr*150, clamped 0.005..0.999, a = (1-t)/(1+t),
// t = tan(1.57 f)); the R channel uses f * (9 p0^2 + 1), so the L/R phase responses differ (width). p6 pan trim:
// L gain 2 - 2(0.3 p6 + 0.35), R gain 2(0.3 p6 + 0.35). Unused stages are cleared; states below 1e-8 are flushed
// at the end of each block. The emulated x87 runs at 53-bit precision, so "extended" maths is double here.
// Oracle-verified: bit-exact (-240 dB) over --random 8 and all 0 / all 1 / each stage count.
#include "effect_base.h"

namespace hs2 {
namespace {

class StereoEnhancer : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.05);  // +0xc tail: trunc(sr * 0.05)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        std::memset(x_, 0, sizeof x_);
        std::memset(y_, 0, sizeof y_);
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        const float gR = float(double(trim_) + double(trim_));
        const float gL = float(2.0 - double(gR));
        for (int k = stages_; k < 3; ++k) x_[0][k] = y_[0][k] = x_[1][k] = y_[1][k] = 0.f;
        const int ns = stages_ < 1 ? 1 : (stages_ > 3 ? 3 : stages_);
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                float v = io[2 * i + c];
                for (int k = 0; k < ns; ++k) {
                    const float y = float((double(y_[c][k]) - double(v)) * double(a_[c][k]) + double(x_[c][k]));
                    x_[c][k] = v;
                    y_[c][k] = y;
                    v = y;
                }
                io[2 * i + c] = float(double(c ? gR : gL) * double(v));
            }
        }
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 3; ++k) {
                if (std::fabs(x_[c][k]) < 1e-8f) x_[c][k] = 0.f;
                if (std::fabs(y_[c][k]) < 1e-8f) y_[c][k] = 0.f;
            }
    }

private:
    static float coef(double f) {
        if (f < double(0.005f)) f = double(0.005f);
        else if (f > double(0.999f)) f = double(0.999f);
        const double t = std::tan(f * double(1.57f));
        return float((1.0 - t) / (t + 1.0));
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: case 2: case 3: case 4: {
                const double w = double(p_[0]) * double(p_[0]) * 9.0 + 1.0;
                for (int k = 0; k < 3; ++k) {
                    const double f =
                        std::exp2(double(p_[2 + k]) * double(4.6052f) * 1.4426950408889634) * double(invSr_) * 150.0;
                    a_[0][k] = coef(f);
                    a_[1][k] = coef(f * w);
                }
                return;
            }
            case 5: stages_ = 1 - int(double(v) * double(-2.9f)); return;
            case 6: trim_ = float(double(v) * double(0.3f) + double(0.35f)); return;
            default: return;
        }
    }
    int stages_ = 1;
    float trim_ = 0.35f;
    float a_[2][3] = {}, x_[2][3] = {}, y_[2][3] = {};
};

}  // namespace

std::unique_ptr<Effect> make_fx_52() { return std::make_unique<StereoEnhancer>(); }

}  // namespace hs2
