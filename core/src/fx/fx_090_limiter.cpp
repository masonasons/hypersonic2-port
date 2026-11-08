// 90 Limiter (ctor FUN_1003ce80, vtable 0x100a9918: setSampleRate FUN_1003cea0, process FUN_1003cf10,
// setParam FUN_1003d230, paramName 0x1003d350, paramDisplay 0x1003d420; init/tempo 0x10002170 is a no-op).
// Input drive (p2: 2*exp(3.4539*p)), stereo-linked peak follower (attack p3, release p4, one-pole coefficients
// 1 - exp(-((1-p)^3*50000+100)/sr) resp. ((1-p)^3*50+1)/sr), gain = out/env when env > 1, output p0 exp(4.6052p-3).
// Quirk: the peak is max(|L|, |R|) where |R| goes through a float store only when it wins.
// The original keeps intermediates on the x87 stack (53-bit precision control); mirrored with double.
// Silence gate: common pattern, tail trunc(sr*0.1f); a skipped block also clears the envelope (+0x88).
// Oracle-verified (also with fx_compare --gated): bit-exact (-240 dB) over --random 8 and all 0 / all 1 / extreme attack-release sets.
#include "effect_base.h"

namespace hs2 {
namespace {

using X = double;  // the original runs the x87 at 53-bit precision (MSVC default control word)

class Limiter : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * double(0.1f));  // +0xc tail: trunc(sr * 0.1f)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        env_ = 0.f;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // Silence gate: the common pattern, but the skip branch (silent, countdown < 0) also clears the envelope (+0x88).
    void processGated(float* lr, int n, bool& silent) override {
        if (silent && gateCount_ < 0) {
            env_ = 0.f;
            return;
        }
        EffectBase::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        for (int i = 0; i < n; ++i) {
            const X a = X(drive_) * X(io[2 * i]);
            const X b = X(drive_) * X(io[2 * i + 1]);
            X pk = std::fabs(a);
            const X bb = std::fabs(b);
            if (bb > pk) pk = X(float(bb));
            const float c = pk > X(env_) ? atk_ : rel_;
            const X e = (pk - X(env_)) * X(c) + X(env_);
            env_ = float(e);
            const X g = e > X(1.f) ? X(out_) / e : X(out_);
            io[2 * i] = float(g * a);
            io[2 * i + 1] = float(g * b);
        }
    }

private:
    static float coef(X p, float k, float c, float invSr) {
        const X q = X(1.f) - p;
        return float(X(1.f) - std::exp(-((q * q * q * X(k) + X(c)) * X(invSr))));
    }
    void onParam(int i, float v) override {
        const X p = v;
        switch (i) {
            case 0: out_ = float(std::exp(p * X(4.6052f) - X(3.f))); return;
            case 2: {
                const X e = std::exp(p * X(3.4539f));
                drive_ = float(e + e);
                return;
            }
            case 3: atk_ = coef(p, 50000.f, 100.f, invSr_); return;
            case 4: rel_ = coef(p, 50.f, 1.f, invSr_); return;
            default: return;
        }
    }
    float out_ = 1, drive_ = 1, atk_ = 0, rel_ = 0, env_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_90() { return std::make_unique<Limiter>(); }

}  // namespace hs2
