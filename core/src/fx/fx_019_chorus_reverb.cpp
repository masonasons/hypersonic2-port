// 19 Chorus + Reverb (ctor FUN_10030830, setSampleRate FUN_100308c0, setParam FUN_10030ce0, process FUN_100309c0;
// vtable 0x100a90b8, paramName 0x10030df0, paramDisplay 0x10030f10; init/tempo 0x10002170 unused).
// A chorus followed (in place) by an embedded Hall Reverb object (ctor FUN_100261f0 = type 1, reused here through
// make_fx_1()). Chorus: mono-sum 50 ms line, L/R fractional taps steered by a triangle LFO (R inverted:
// depth - tri) re-targeted every 256 samples like the Chorus (type 20), base delay sr*0.006 + 1.
// p0 mix, p1/p2 rate (last set wins), p3 depth, p4 Cho<>Rev: chorus mix (1 - p4^2) p0, reverb mix p0 p4.
// p5 rev time, p6 pre delay, p7 damping, p8 lo cut, p9 hi cut are forwarded as Hall Reverb params 2..6
// (Hall params 1, 7.. keep their defaults).
// x87 intermediates are mirrored with double (the original runs the FPU at 53-bit precision, MSVC default).
// Oracle-verified: worst -131 dB over --random 8 + extremes 0/1 + hand sets; the chorus alone (p4 = 0) is bit-exact,
// the residual is the Hall Reverb port (type 1 gives the same -130.6 dB with the same reverb params). NOTE: the
// stock fx_compare cannot construct this effect (the ctor uses SEH via fs:[0], unmapped in hs2emu), so it was run
// with a copy of fx_compare that replicates the ctor (base ctor FUN_10025400, fields, Hall ctor FUN_100261f0 at +0xb8).
#include "effect_base.h"

namespace hs2 {
namespace {

using LD = double;  // x87 intermediates (MSVC runs the FPU at 53-bit precision)

class ChorusReverb : public EffectBase {
public:
    ChorusReverb() : rev_(make_fx_1()) {}
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        rev_->setSampleRate(sr);
        iL_ = iR_ = w_ = 0;
        count_ = 1;
        velL_ = velR_ = 0;
        base_ = float(LD(sr) * LD(0.006f) + 1.0);
        fracL_ = fracR_ = 0;
        ph_ = 0;
        L_ = int(double(sr) * double(0.05f));
        buf_.assign(size_t(L_) + 2, 0.f);
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const LD m = mix_;
        const float dry = float(1.0 - m * m);
        const float wet = float(((m + m) - m * m) * 0.5);
        const int L1 = L_ - 1;
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                const LD p = LD(inc_) + ph_;
                ph_ = float(p);
                if (p > 2.0) ph_ = float(p - 2.0);
                const LD tri = (ph_ <= 1.f ? LD(ph_) : 2.0 - ph_) * depth_;
                int d = iL_ - w_;
                if (d < 0) d += L_;
                LD v = (((tri + base_) - fracL_) - d) * 0.00390625 - 1.0;
                velL_ = v > 0.0 ? 0.f : float(v);
                d = iR_ - w_;
                if (d < 0) d += L_;
                v = ((((LD(depth_) - tri) + base_) - fracR_) - d) * 0.00390625 - 1.0;
                velR_ = v > 0.0 ? 0.f : float(v);
                count_ = 256;
            }
            float* f = io + 2 * i;
            const float inL = f[0], inR = f[1];
            LD a = b[iL_];
            f[0] = float(((LD(b[iL_ + 1]) - a) * fracL_ + a) * wet + LD(inL) * dry);
            LD fr = LD(velL_) + fracL_;
            fracL_ = float(fr);
            if (fr < 0.0) {
                do {
                    if (--iL_ < 0) iL_ = L1;
                    fracL_ = fracL_ + 1.f;
                } while (fracL_ < 0.f);
            }
            a = b[iR_];
            f[1] = float(((LD(b[iR_ + 1]) - a) * fracR_ + a) * wet + LD(inR) * dry);
            fr = LD(velR_) + fracR_;
            fracR_ = float(fr);
            if (fr < 0.0) {
                do {
                    if (--iR_ < 0) iR_ = L1;
                    fracR_ = fracR_ + 1.f;
                } while (fracR_ < 0.f);
            }
            --w_;
            b[w_ + 1] = inR + inL;
            if (w_ < 0) {
                b[L_] = b[0];
                w_ = L1;
            }
        }
        rev_->process(io, n);
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: case 4:
                rev_->setParam(0, p_[0] * p_[4]);
                mix_ = float((1.0 - LD(p_[4]) * p_[4]) * p_[0]);
                return;
            case 1: case 2: {
                const LD r = std::exp(LD(v) * LD(5.991464f) - LD(2.995732f));
                rate_ = float(r);
                inc_ = float(r * invSr_ * 256.0);
                [[fallthrough]];
            }
            case 3: {
                LD f = LD(0.02f) / LD(rate_);
                if (!(f <= LD(0.024f))) f = LD(0.024f);
                depth_ = float(f * sr_ * p_[3] * p_[3] * 0.5);
                return;
            }
            case 5: case 6: case 7: case 8: case 9: rev_->setParam(i - 3, v); return;
            default: return;
        }
    }
    std::unique_ptr<Effect> rev_;
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, iL_ = 0, iR_ = 0, count_ = 1;
    float mix_ = 0, rate_ = 1.f, inc_ = 0, depth_ = 0, base_ = 1, ph_ = 0;
    float fracL_ = 0, fracR_ = 0, velL_ = 0, velR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_19() { return std::make_unique<ChorusReverb>(); }

}  // namespace hs2
