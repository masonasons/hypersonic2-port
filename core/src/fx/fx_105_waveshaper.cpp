// 105 Waveshaper (ctor FUN_10040390, setSampleRate FUN_100403c0, setParam FUN_10040570, process FUN_10040430).
// Envelope follower env += (max(|L|,|R|) + 0.01 - env)*k (k = 1-exp(-exp(12-24*p3)/sr)); the input is normalised
// by the envelope and wrapped: a = fmod(x*drive/env + 101, 2) - 1, y = (a - a^3)*env, out = y*p4^2 mixed with x.
// p0 mix, p2 drive p^3*33+0.5, p3 rate, p4 out. Quirk kept: the left channel uses the gain drive/env at x87
// precision, the right channel its float-rounded copy; the left shaped value is spilled as float, the right is not.
// Silence gating: none; process ignores the silent flag (always processes, never clears it).
// Oracle-verified: bit-exact (-240 dB) over --random 8 + extremes; --gated: -240 dB, 0 flag mismatches.
#include "effect_base.h"

namespace hs2 {
namespace {

class Waveshaper : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        reapplyParams();
        env_ = 0;
    }
    // The original process never reads or writes the silent flag (and has no tail countdown): it always processes
    // and leaves the flag as it was.
    void processGated(float* lr, int frames, bool&) override { process(lr, frames); }
    void process(float* io, int n) override {
        if (mix_ == 0.f) return;
        for (int i = 0; i < n; ++i) {
            const float L = io[2 * i], R = io[2 * i + 1];
            float m = std::fabs(L);
            if (std::fabs(R) > m) m = std::fabs(R);
            env_ = float(((double(m) + double(0.01f)) - env_) * k_ + env_);
            const double g = 1.0 / double(env_) * drive_;
            const float gf = float(g);
            const double a = std::fmod(g * L + 101.0, 2.0) - 1.0;
            const float yL = float((a - a * a * a) * env_);
            const double b = std::fmod(double(gf) * R + 101.0, 2.0) - 1.0;
            const double yR = (b - b * b * b) * env_;
            io[2 * i] = float((double(yL) * out_ - L) * mix_ + L);
            io[2 * i + 1] = float((yR * out_ - R) * mix_ + R);
        }
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: drive_ = float(double(v) * v * v * 33.0 + 0.5); return;
            case 3: {
                const double r = std::exp(12.0 - double(v) * 24.0);
                k_ = float(1.0 - std::exp(-(r * invSr_)));
                return;
            }
            case 4: out_ = float(double(v) * v); return;
            default: return;
        }
    }
    float mix_ = 0.5f, drive_ = 0.5f, k_ = 0, out_ = 0, env_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_105() { return std::make_unique<Waveshaper>(); }

}  // namespace hs2
