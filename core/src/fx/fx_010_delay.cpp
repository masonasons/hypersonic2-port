// 10 Delay (ctor FUN_1002bf80, setSampleRate FUN_1002bfd0, setParam FUN_1002c4e0, process FUN_1002c0a0,
// tempo FUN_1002d850). One mono feedback line (1.51 s), stereo taps. p0 mix (wet 2m-m^2, dry 1-m^2), p1 sync,
// p2 time, p3 feedback (taken from the longer tap), p4 L/R offset, p5 damping. Also used by 12 Long Delay (4.01 s).
// x87 intermediates are mirrored with double (the original runs the FPU at 53-bit precision, MSVC default).
// (Ghidra's decompile suggests the feedback tap is only refreshed every 4th sample; the original does it every sample.)
// Oracle-verified: bit-exact (-240 dB floor), including p2 = 0.218 / 0.276 where rounding the time factor to float
// moved the taps by one sample (an earlier version of this port measured -2.5 dB there).
#include "effect_base.h"

namespace hs2 {
namespace {

class Delay : public EffectBase {
public:
    explicit Delay(float seconds) : seconds_(seconds) {
        p_[4] = 0.5f;
        p_[5] = 1.f;
        p_[6] = 0.5f;
    }
    void setSampleRate(float sr) override {
        gate_ = Gate::Detect;  // keeps running while its output is audible (FUN_10025480)
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(seconds_));  // x87: 44100*1.51f -> 66590
        buf_.assign(L_, 0.f);
        w_ = 0;
        s_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_1002d850): beat = 1/b (samples per beat), then re-applies param 2. 0 until wired (TODO).
    void setTempo(float, float b) override {
        beat_ = b != 0.f ? 1.f / b : 0.f;
        setParam(2, p_[2]);
    }
    void process(float* io, int n) override {
        if (buf_.empty()) return;
        const int L1 = L_ - 1;
        int w = w_;
        int rl = tapL_ + w, rr = tapR_ + w;
        if (L1 < rl) rl -= L_;
        if (L1 < rr) rr -= L_;
        const bool fbFromR = tapL_ <= tapR_;
        const float m = mix_;
        const double m2 = double(m) * m;  // x87 (53-bit): m^2 kept in a register
        const float wet = float((double(m) + m) - m2), dry = float(1.0 - m2);
        for (int i = 0; i < n; ++i) {
            float* f = io + 2 * i;
            const float inL = f[0], inR = f[1];
            --rl;
            const float vl = buf_[rl + 1];
            if (rl < 0) rl = L1;
            --rr;
            const float vr = buf_[rr + 1];
            if (rr < 0) rr = L1;
            f[0] = float(double(dry) * inL + double(wet) * vl);
            f[1] = float(double(dry) * inR + double(wet) * vr);
            const float fb = fbFromR ? vr : vl;
            s_ = float((((double(inL) + inR) * 0.5 + double(fb) * fbk_ + double(1e-8f)) - s_) * damp_ + s_);
            --w;
            buf_[w + 1] = s_;
            if (w < 0) w = L1;
        }
        w_ = w;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 3: fbk_ = v * 0.995f; return;
            case 5:
                damp_ = 1.f;
                if (v < 0.98f)
                    damp_ = float(1.0 - std::exp(-((double(v) * v * v * 95000.0 + 5000.0) * double(invSr_))));
                return;
            case 1: case 2: case 4: break;
            default: return;
        }
        // x87: the time factor is stored to this+0x90 with fst but the extended value is what scales the length
        // (rounding it to float moves the taps by one sample for some p2, e.g. 0.218 in the 1.51 s Delay).
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
    float seconds_;
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, tapL_ = 0, tapR_ = 0;
    float s_ = 0, mix_ = 0.5f, fbk_ = 0, damp_ = 1, beat_ = 0.f;
};

}  // namespace

// Shared with 12 Long Delay (same code, 4.01 s line).
std::unique_ptr<Effect> makeDelay(float seconds) { return std::make_unique<Delay>(seconds); }

std::unique_ptr<Effect> make_fx_10() { return makeDelay(1.51f); }

}  // namespace hs2
