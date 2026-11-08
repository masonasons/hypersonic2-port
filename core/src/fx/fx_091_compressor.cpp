// 91 Compressor (ctor FUN_1003d490, vtable 0x100a9944: setSampleRate FUN_1003d4b0, process FUN_1003d520,
// setParam FUN_1003d950, paramName 0x1003daa0, paramDisplay 0x1003dba0; init/tempo 0x10002170 is a no-op).
// Stereo-linked peak detector: above the envelope it attacks one-pole (p4: 1 - exp(-exp(9-4p)/sr)) and resets the
// release multiplier to 1; below it the envelope is multiplied by a release factor that itself glides from 1 towards
// exp(-exp(3.6-2.6p)/sr) (p5). Gain = out / ((env/thr)*ratio + 1 - ratio) above the threshold
// (p2 thr exp(4.6p-3.5), p3 ratio p^3*1.2, p0 output exp(4.6p-2.3)).
// The original keeps intermediates on the x87 stack (53-bit precision control); mirrored with double.
// Silence gate: common pattern, tail trunc(sr*0.1f); a skipped block also clears the envelope (+0x7c).
// Oracle-verified (also with fx_compare --gated): bit-exact (-240 dB) over --random 8 and all 0 / all 1 / extreme attack-release sets.
#include "effect_base.h"

namespace hs2 {
namespace {

using X = double;  // the original runs the x87 at 53-bit precision (MSVC default control word)

class Compressor : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * double(0.1f));  // +0xc tail: trunc(sr * 0.1f)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        env_ = 0.f;
        relMul_ = 0.f;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // Silence gate: the common pattern, but the skip branch (silent, countdown < 0) also clears the envelope (+0x7c).
    void processGated(float* lr, int n, bool& silent) override {
        if (silent && gateCount_ < 0) {
            env_ = 0.f;
            return;
        }
        EffectBase::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        const float invThr = float(X(1.f) / X(thr_));
        const float relGlide = float(X(1.f) - X(rel_));
        for (int i = 0; i < n; ++i) {
            const float l = io[2 * i], r = io[2 * i + 1];
            const float pk = std::max(std::fabs(l), std::fabs(r));
            if (pk > env_) {
                relMul_ = 1.f;
                env_ = float((X(pk) - X(env_)) * X(atk_) + X(env_));
            } else {
                env_ = float(X(relMul_) * X(env_));
                relMul_ = float((X(rel_) - X(relMul_)) * X(relGlide) + X(relMul_));
            }
            X g = out_;
            if (env_ > thr_) g = X(out_) / ((X(invThr) * X(env_) * X(ratio_) + X(1.f)) - X(ratio_));
            io[2 * i] = float(g * X(l));
            io[2 * i + 1] = float(g * X(r));
        }
    }

private:
    void onParam(int i, float v) override {
        const X p = v;
        switch (i) {
            case 0: out_ = float(std::exp(p * X(4.6f) - X(2.3f))); return;
            case 2: thr_ = float(std::exp(p * X(4.6f) - X(3.5f))); return;
            case 3: ratio_ = float(p * p * p * X(1.2f)); return;
            case 4: atk_ = float(X(1.f) - std::exp(-(std::exp(X(9.f) - p * X(4.f)) * X(invSr_)))); return;
            case 5: rel_ = float(std::exp(-(std::exp(X(3.6f) - p * X(2.6f)) * X(invSr_)))); return;
            default: return;
        }
    }
    float out_ = 1, thr_ = 1, ratio_ = 0, atk_ = 0, rel_ = 0, env_ = 0, relMul_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_91() { return std::make_unique<Compressor>(); }

}  // namespace hs2
