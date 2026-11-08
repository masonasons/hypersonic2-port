// 16 Diffuser Delay (ctor FUN_100437e0, vtable 0x100a9d18, process FUN_1002c8b0, paramName 0x1002cbf0,
// paramDisplay 0x1002c730). Shares setSampleRate FUN_1002bfd0 (1.51 s), setParam FUN_1002c4e0 and tempo
// FUN_1002d850 with 10 Delay. p0 mix, p1 sync, p2 time, p3 feedback (from the longer tap), p4 L/R offset,
// p5 damping, p6 diffusion (default 0.5).
// Process: like the Delay, but after the damped feedback sample s is written, four in-place lattice allpass stages
// (coefficient 0.5) run through the same delay line at offsets D*{0.3747, 0.2823, 0.7309, 1} + 1 after the write
// head, D = min(tapL, tapR) * p6^2 * 0.4, so the echoes smear progressively. x87 intermediates are mirrored
// with double (the original runs the FPU at 53-bit precision, MSVC default; so does the oracle).
// Oracle-verified: bit-exact (-240 dB floor) over fx_compare --random 8 + extremes 0/1 + hand sets.
#include "effect_base.h"

namespace hs2 {
namespace {

class DiffuserDelay : public EffectBase {
public:
    DiffuserDelay() {
        p_[4] = 0.5f;
        p_[5] = 1.f;
        p_[6] = 0.5f;
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(1.51f));
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
    // Silence gating: this process (FUN_1002c8b0, unlike the Delay's) never reads or writes the silent flag or the
    // countdown: it always runs and leaves the flag as it was.
    void processGated(float* lr, int frames, bool&) override { process(lr, frames); }
    void process(float* io, int n) override {
        using LD = double;  // x87 intermediates (MSVC runs the FPU at 53-bit precision)
        if (buf_.empty()) return;
        const int L = L_, L1 = L_ - 1;
        int w = w_;
        int rl = tapL_ + w, rr = tapR_ + w;
        if (L1 < rl) rl -= L;
        if (L1 < rr) rr -= L;
        const int mn = tapL_ < tapR_ ? tapL_ : tapR_;
        const LD D = LD(mn) * p_[6] * p_[6] * LD(0.4f);
        int i1 = w - int(D * LD(-0.3747f)) + 1;
        if (L1 < i1) i1 -= L;
        int i2 = i1 - int(D * LD(-0.2823f)) + 1;
        if (L1 < i2) i2 -= L;
        int i3 = i2 - int(D * LD(-0.7309f)) + 1;
        if (L1 < i3) i3 -= L;
        int i4 = int(D) + i3 + 1;
        if (L1 < i4) i4 -= L;
        const bool fbFromL = tapL_ > tapR_;
        const float m = mix_;
        const LD m2 = LD(m) * m;  // stays in an x87 register
        const float dry = float(1.0 - m2), wet = float((LD(m) + m) - m2);
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            float* f = io + 2 * i;
            const float inL = f[0], inR = f[1];
            const float vl = b[rl];
            if (--rl < 0) rl = L1;
            f[0] = float(LD(dry) * inL + LD(wet) * vl);
            const LD sumIn = LD(inL) + inR;
            --rr;
            const float vr = b[rr + 1];
            if (rr < 0) rr = L1;
            f[1] = float(LD(dry) * inR + LD(wet) * vr);
            --w;
            const LD s = ((LD(fbk_) * (fbFromL ? vl : vr) + sumIn * 0.5 + LD(1e-8f)) - s_) * damp_ + s_;
            s_ = float(s);
            const LD a1 = LD(b[i1]) - 0.5 * s;
            const float a1f = float(a1);
            const LD h1 = a1 * 0.5;
            const float h1f = float(h1);
            b[w + 1] = float(h1 + s);
            if (w < 0) w = L1;
            --i1;
            const LD a2 = LD(b[i2]) - h1f;
            const float a2f = float(a2);
            const LD h2 = a2 * 0.5;
            b[i1 + 1] = float(LD(a1f) + h2);
            if (i1 < 0) i1 = L1;
            --i2;
            const float a3f = float(LD(b[i3]) - h2);
            const LD h3 = LD(a3f) * 0.5;
            b[i2 + 1] = float(LD(a2f) + h3);
            if (i2 < 0) i2 = L1;
            --i3;
            const LD a4 = LD(b[i4]) - h3;
            b[i3 + 1] = float(0.5 * a4 + a3f);
            if (i3 < 0) i3 = L1;
            b[i4] = float(a4);
            if (--i4 < 0) i4 = L1;
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
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, tapL_ = 0, tapR_ = 0;
    float s_ = 0, mix_ = 0.5f, fbk_ = 0, damp_ = 1, beat_ = 0.f;
};

}  // namespace

std::unique_ptr<Effect> make_fx_16() { return std::make_unique<DiffuserDelay>(); }

}  // namespace hs2
