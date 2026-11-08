// 11 Stereo Delay (ctor FUN_1002d740, setSampleRate FUN_1002d780, setParam FUN_1002ddf0, process FUN_1002d880,
// tempo FUN_1002d850; vtable 0x100a8ed4, paramName 0x1002dff0, paramDisplay 0x1002e0a0).
// Stereo (interleaved) 1.5 s line, one tap per channel, each channel with its own one-pole damped feedback.
// p0 mix (wet 2m-m^2, dry 1-m^2), p1 sync, p2 time, p3 feedback in [-0.99, 0.99]: positive = each channel feeds
// itself, negative = cross (ping-pong; the wet output then also takes the opposite channel's tap), p4 L/R offset,
// p5 damping. Defaults p3 0.75, p4 0.5, p5 1. The 1e-8 denormal guard is added on the store only.
// x87 intermediates are mirrored with double (the original runs the FPU at 53-bit precision, MSVC default).
// Oracle-verified: bit-exact (-240 dB floor) over fx_compare --random 8 plus extremes 0/1, sync and cross-feedback
// sets and p2 = 0.218 / 0.276 (where rounding the time factor to float would move the taps).
#include "effect_base.h"

namespace hs2 {
namespace {

class StereoDelay : public EffectBase {
public:
    StereoDelay() {
        p_[3] = 0.75f;
        p_[4] = 0.5f;
        p_[5] = 1.f;
    }
    void setSampleRate(float sr) override {
        gate_ = Gate::Detect;  // keeps running while its output is audible (FUN_10025480)
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(1.5f));
        buf_.assign(size_t(L_) * 2, 0.f);
        w_ = 0;
        sL_ = sR_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_1002d850): beat = 1/b, then re-applies param 2.
    void setTempo(float, float b) override {
        beat_ = 1.f / b;
        setParam(2, p_[2]);
    }
    void process(float* io, int n) override {
        if (buf_.empty()) return;
        const int L1 = L_ - 1;
        int w = w_;
        int rl = tapL_ + w, rr = tapR_ + w;
        if (L1 < rl) rl -= L_;
        if (L1 < rr) rr -= L_;
        const bool cross = fbk_ <= 0.f;
        const float a = std::fabs(fbk_);
        const float m = mix_;
        const double m2 = double(m) * m;  // x87 (53-bit precision): m^2 stays in a register
        const float dry = float(1.0 - m2), wet = float((double(m) + m) - m2);
        const float d = damp_;
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            float* f = io + 2 * i;
            const float inL = f[0], inR = f[1];
            --rl;
            const float vl = b[2 * (rl + 1)];
            if (rl < 0) rl = L1;
            const float vr = b[2 * rr + 1];
            if (--rr < 0) rr = L1;
            const float tl = cross ? vr : vl, tr = cross ? vl : vr;
            --w;
            double s = ((double(a) * tl + inL) - sL_) * d + sL_;
            sL_ = float(s);
            b[2 * (w + 1)] = float(s + double(1e-8f));
            s = ((double(a) * tr + inR) - sR_) * d + sR_;
            sR_ = float(s);
            b[2 * (w + 1) + 1] = float(s + double(1e-8f));
            if (w < 0) w = L1;
            f[0] = float(double(wet) * tl + double(inL) * dry);
            f[1] = float(double(wet) * tr + double(inR) * dry);
        }
        w_ = w;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 3: fbk_ = float(double(v) * 1.98f - 0.99f); return;
            case 5:
                damp_ = 1.f;
                if (v < 0.98f)
                    damp_ = float(1.0 - std::exp(-((double(v) * v * v * 95000.0 + 5000.0) * double(invSr_))));
                return;
            case 1: case 2: case 4: break;
            default: return;
        }
        double t;
        if (p_[1] <= 0.5f) {
            t = ((double)p_[2] * p_[2] * (double)0.999f + (double)0.001f) * (double)(L_ - 1);
        } else {  // tempo sync: samples-per-beat / note value, halved until it fits
            t = (double)beat_ / (double)noteValue(1.f - p_[2]);
            while ((double)(L_ - 1) < t) t *= 0.5;
        }
        if (p_[4] <= 0.5f) {
            tapR_ = int(t);
            tapL_ = int(((double)p_[4] + 0.5) * t);
        } else {
            tapL_ = int(t);
            tapR_ = int((1.5 - (double)p_[4]) * t);
        }
    }
    // FUN_100489b0: note length in beats, index trunc(v*20.9)
    static float noteValue(float v) {
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, tapL_ = 0, tapR_ = 0;
    float sL_ = 0, sR_ = 0, mix_ = 0.5f, fbk_ = 0, damp_ = 1, beat_ = 0.f;
};

}  // namespace

std::unique_ptr<Effect> make_fx_11() { return std::make_unique<StereoDelay>(); }

}  // namespace hs2
