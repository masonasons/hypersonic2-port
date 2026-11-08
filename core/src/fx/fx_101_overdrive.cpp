// 101 Overdrive (ctor FUN_1003fbf0, setSampleRate FUN_1003fc20, setParam FUN_10040200, process FUN_1003fcc0).
// Per channel: u = (x*drive + bias)^2 - 0.5; for u > 0 soft-saturate u -= u^2 (u > 0.5 -> 0.25); subtract the
// static offset of the bias point (c) and a DC tracker s (one pole, k = 1-exp(-300/sr)): e = u - (c + s),
// s += e*k; out = e*p4^2 mixed with x by p0. p2 drive exp(3.5p), p3 bias. Everything but s stays in x87 registers.
// Oracle-verified: bit-exact (-240 dB) over --random 8 + extremes.
#include "effect_base.h"

namespace hs2 {
namespace {

class Overdrive : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        const double inv = 1.0 / double(sr);
        invSr_ = float(inv);
        dc_[0] = dc_[1] = 0;
        k_ = float(1.0 - std::exp(inv * -300.0));
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (mix_ == 0.f) return;
        for (int i = 0; i < 2 * n; ++i) {
            float& s = dc_[i & 1];
            const double x = io[i];
            double u = x * drive_ + bias_;
            u = u * u - 0.5;
            if (u > 0.0) u = u > 0.5 ? 0.25 : u - u * u;
            const double e = u - (double(off_) + s);
            s = float(e * k_ + s);
            io[i] = float((e * out_ - x) * mix_ + x);
        }
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: drive_ = float(std::exp(double(v) * 3.5)); return;
            case 3: {
                bias_ = v;
                double u = double(v) * v - 0.5;
                if (u > 0.0) u -= u * u;
                off_ = float(u);
                return;
            }
            case 4: out_ = float(double(v) * v); return;
            default: return;
        }
    }
    float mix_ = 0.5f, drive_ = 1, bias_ = 0, off_ = 0, out_ = 0, k_ = 0;
    float dc_[2] = {0, 0};
};

}  // namespace

std::unique_ptr<Effect> make_fx_101() { return std::make_unique<Overdrive>(); }

}  // namespace hs2
