// 50 Pan / Tremolo (ctor FUN_10033c20, vtable 0x100a9274: setSampleRate FUN_10033c40, process FUN_10033cd0,
// setParam FUN_10033f00; init/tempo 0x10002170 is a no-op).
// Triangle LFO (phase 0..2, folded to 0..1) per channel, R offset by 2 p3 (p3 = 0.5 -> auto-pan). Every 256
// samples the target gain v = clamp(tri * (|s| + 3) + min(-s, 0), 0, 0.125 s + 1.5) with shape s = 6 (p4 - 0.25)
// is turned into a linear ramp of (v - 1) p0 + 1 over the next 256 samples. p0 depth (0 = bypass, untouched),
// p1/p2 rate (e^(5.991464 p - 2.995732) Hz, last set wins), p3 phase, p4 shape.
// The emulated x87 runs at 53-bit precision, so "extended" maths is double here.
// Oracle-verified: bit-exact (-240 dB) over --random 8 and all 0 / all 1 / hand-picked shapes.
#include "effect_base.h"

namespace hs2 {
namespace {

class PanTremolo : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.05);  // +0xc tail: trunc(sr * 0.05)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        count_ = 1;
        ph_ = 0.f;
        gL_ = gLinc_ = gR_ = gRinc_ = 0.f;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // Gate (FUN_10033cd0): the depth == 0 bypass returns before the silence gate, so the flag stays set.
    void processGated(float* lr, int n, bool& silent) override {
        if (depth_ == 0.f) return;
        Effect::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        if (depth_ == 0.f) return;
        const float A = float(std::fabs(double(shape_)) + 3.0);
        const float cmax = float(double(shape_) * 0.125 + 1.5);
        const double off = shape_ > 0.f ? -double(shape_) : 0.0;
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                const double p = double(rate_) + double(ph_);
                ph_ = float(p);
                if (!(p < 2.0)) ph_ = float(p - 2.0);
                const double t = ph_ > 1.f ? 2.0 - double(ph_) : double(ph_);
                gLinc_ = float((((clampv(t * double(A) + off, cmax) - 1.0) * double(depth_) + 1.0) - double(gL_)) *
                               0.00390625);
                double u = double(phase_) + double(ph_);
                if (u > 2.0) u -= 2.0;
                if (u > 1.0) u = 2.0 - u;
                gRinc_ = float((((clampv(u * double(A) + off, cmax) - 1.0) * double(depth_) + 1.0) - double(gR_)) *
                               0.00390625);
                count_ = 256;
            }
            double g = double(gLinc_) + double(gL_);
            gL_ = float(g);
            io[2 * i] = float(g * double(io[2 * i]));
            g = double(gRinc_) + double(gR_);
            gR_ = float(g);
            io[2 * i + 1] = float(g * double(io[2 * i + 1]));
        }
    }

private:
    static double clampv(double v, float cmax) {
        if (v < 0.0) return 0.0;
        if (v > double(cmax)) return double(cmax);
        return v;
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: depth_ = v; return;
            case 1: case 2: {
                const double hz = std::exp2((double(v) * double(5.991464f) - double(2.995732f)) * 1.4426950408889634);
                rate_ = float(hz * double(invSr_) * 512.0);
                return;
            }
            case 3: phase_ = float(double(v) + double(v)); return;
            case 4: shape_ = float((double(v) - 0.25) * 6.0); return;
            default: return;
        }
    }
    int count_ = 1;
    float depth_ = 0.5f, rate_ = 0, phase_ = 1, shape_ = 0, ph_ = 0;
    float gL_ = 0, gLinc_ = 0, gR_ = 0, gRinc_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_50() { return std::make_unique<PanTremolo>(); }

}  // namespace hs2
