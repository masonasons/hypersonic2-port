// 93 Punch (ctor FUN_1003e240, setSampleRate FUN_1003e260, setParam FUN_1003e810, process FUN_1003e3e0,
// coefficient recalc FUN_1003e2f0 run from process when p0/p2/p3 changed).
// Linked-stereo peak compressor: env = max(|L|,|R|), else decays env*(1-440/sr). Above the threshold
// thr = exp(-6.9078*p2)/4 the target gain is a/env + b with r = (1-exp(d*0.95*p3))/(1-thr), a = thr*r, b = 1-r
// (d = -6.9078*p2, clamped <= -1e-4); below it 1. The gain glides with attack (falling) / release (rising)
// one-pole coefficients 2200/(1000.99^p4-0.99)/sr and 2200/10000^p5/sr (both <= 0.99).
// Output = x * gain * makeup, makeup = exp((p0-0.5)*4.60517)/(a+b).
// Oracle-verified: bit-exact (-240 dB) over --random 8 + extremes.
#include "effect_base.h"

namespace hs2 {
namespace {

class Punch : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        const double inv = 1.0 / double(sr);
        invSr_ = float(inv);
        gain_ = 1.f;
        env_ = 0.f;
        decay_ = float(1.0 - inv * 440.0);
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (dirty_) recalc();
        for (int i = 0; i < n; ++i) {
            const float L = io[2 * i], R = io[2 * i + 1];
            double m = std::fabs(L);
            if (std::fabs(R) > m) m = std::fabs(R);
            if (!(m > double(env_))) m = double(decay_) * env_;
            env_ = float(m);
            double t;
            if (m < thr_) t = 1.0;
            else t = double(a_) / m + b_;
            const double c = t <= double(gain_) ? atk_ : rel_;
            const double g = (t - gain_) * c + gain_;
            gain_ = float(g);
            const double gg = g * makeup_;
            io[2 * i] = float(gg * L);
            io[2 * i + 1] = float(gg * R);
        }
        if (env_ < 1e-08f) env_ = 0;
        if (gain_ < 1e-08f) gain_ = 0;
    }

private:
    void recalc() {
        dirty_ = false;
        double d = double(p_[2]) * -6.90775527898214;
        if (d > -0.0001) d = -0.0001;
        const double thr = std::exp(d) * 0.25;
        const double one_m = 1.0 - double(p_[3]) * double(0.95f);
        const double e2 = std::exp((1.0 - one_m) * d);
        const double r = (1.0 - e2) / (1.0 - thr);
        thr_ = float(thr);
        const double a = thr * r;
        a_ = float(a);
        const double b = 1.0 - r;
        b_ = float(b);
        const double ex = std::exp((double(p_[0]) - 0.5) * double(4.60517f));
        makeup_ = float(1.0 / (b + a) * ex);
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: case 2: case 3: dirty_ = true; return;
            case 4: {
                double c = 2200.0 / (std::exp(std::log(1000.99) * v) - 0.99) * invSr_;
                if (c > double(0.99f)) c = double(0.99f);
                atk_ = float(c);
                return;
            }
            case 5: {
                double c = 2200.0 / std::exp(std::log(10000.0) * v) * invSr_;
                if (c > double(0.99f)) c = double(0.99f);
                rel_ = float(c);
                return;
            }
            default: return;
        }
    }
    bool dirty_ = true;
    float env_ = 0, decay_ = 0, thr_ = 0, a_ = 0, b_ = 1, gain_ = 1, atk_ = 0, rel_ = 0, makeup_ = 1;
};

}  // namespace

std::unique_ptr<Effect> make_fx_93() { return std::make_unique<Punch>(); }

}  // namespace hs2
