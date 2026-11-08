// 14 Ducking Delay (ctor FUN_1002cc60, setSampleRate FUN_1002ccc0, setParam FUN_1002d070, process FUN_1002cdd0,
// tempo FUN_1002cda0, sidechain-pointer setter FUN_1002ccb0; vtable 0x100a8e7c, paramName 0x1002d3e0,
// paramDisplay 0x1002d520).
// The Delay (1.51 s mono feedback line, stereo taps, same time/sync/offset maths) with a ducker on the wet signal:
// both taps are scaled by g = env + c, c = min(2 p7, 1) (p7 <= 0.498), and the envelope env glides towards
// 1 - 2 p7 (attack coef from p9) while |inL + inR| > threshold p8^3 (0.03 when p8 < 0.01), else back to 0
// (release coef from p10). The feedback takes the ducked longer tap; the line input is lo-cut by subtracting
// p5 x a one-pole low-pass ((1 + p5) 2500 / sr). p0 mix (wet (2m - m^2)/2, dry 1 - m^2), p1 sync, p2 time,
// p3 feedback (x 0.999), p4 L/R offset, p5 lo damp, p6 damping, p7 depth, p8 threshold, p9 attack, p10 release.
// Defaults p4 0.5, p6 1.
// Pitch source (vtable +8 FUN_1002ccb0, this+0x7c = &part note pitch): when connected, the threshold is scaled per
// block by 10 while no note is held (pitch <= -9) and by 0.1 while one is (ducks harder). BUT setSampleRate clears
// the pointer, and the element connects it before setSampleRate, so in the plugin it is normally inert; ported
// faithfully (setPitchSource stores it, setSampleRate drops it).
// Oracle-verified: bit-exact (-240 dB floor) over fx_compare --random 8 + extremes 0/1 + hand sets, with and without
// --pitch; the pitch path itself (source connected after setSampleRate, changes the output by -2.5..-43 dB) is also
// bit-exact (scratch harness).
#include "effect_base.h"

namespace hs2 {
namespace {

using LD = double;  // x87 intermediates (MSVC runs the FPU at 53-bit precision)

class DuckingDelay : public EffectBase {
public:
    DuckingDelay() {
        p_[4] = 0.5f;
        p_[6] = 1.f;
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(1.51f));
        buf_.assign(size_t(L_), 0.f);
        w_ = 0;
        s_ = lo_ = env_ = 0;
        pitch_ = nullptr;  // FUN_1002ccc0 zeroes this+0x7c
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_1002cda0): beat = 1/b, then re-applies param 2.
    // Silence gating (process's flag arg, FUN_1002cdd0): reset to the tail length while the input is live; while
    // silent count down (return once below 0) with the flag left set, and if the output is still audible
    // (FUN_10025480) reload the countdown with tail + line length (+0xc + this+0x84) and clear the flag.
    void processGated(float* lr, int frames, bool& silent) override {
        if (!silent) {
            gateCount_ = gateLen_;
        } else {
            if (gateCount_ < 0) return;
            gateCount_ -= frames;
        }
        process(lr, frames);
        if (silent && hasSignal(lr, frames)) {
            gateCount_ = gateLen_ + L_;
            silent = false;
        }
    }
    void setPitchSource(const float* p) override { pitch_ = p; }  // FUN_1002ccb0
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
        const bool fbFromL = tapL_ > tapR_;
        const LD m = mix_;
        const float dry = float(1.0 - m * m);
        const float wet = float(((m + m) - m * m) * 0.5);
        double thr = thr_;  // stays in an x87 register
        if (pitch_) thr = (*pitch_ <= -9.f ? 10.0 : double(0.1f)) * thr;
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            float* f = io + 2 * i;
            const float g = env_ + depthOfs_;
            const float inL = f[0], inR = f[1];
            --rl;
            const float vl = g * b[rl + 1];
            if (rl < 0) rl = L1;
            --rr;
            f[0] = float(LD(dry) * inL + LD(wet) * vl);
            const LD sum = LD(inL) + inR;
            const float vr = g * b[rr + 1];
            if (rr < 0) rr = L1;
            f[1] = float(LD(dry) * inR + LD(wet) * vr);
            if (std::fabs(sum) > thr)
                env_ = float((LD(target_) - env_) * att_ + env_);
            else
                env_ = float((LD(1e-8f) - env_) * rel_ + env_);
            --w;
            const LD s = ((LD(fbFromL ? vl : vr) * fbk_ + sum) - s_) * damp_ + s_;
            s_ = float(s);
            const LD lo = (s - lo_) * loC_ + lo_;
            lo_ = float(lo);
            b[w + 1] = float(s - lo * loAmt_);
            if (w < 0) w = L1;
        }
        w_ = w;
        if (std::fabs(s_) < 1e-8f) s_ = 0.f;
        if (std::fabs(lo_) < 1e-8f) lo_ = 0.f;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2: case 4: break;
            case 3: fbk_ = v * 0.999f; return;
            case 5:
                loAmt_ = v;
                loC_ = float((LD(v) + 1.0) * 2500.0 * invSr_);
                return;
            case 6:
                damp_ = 1.f;
                if (v < 0.98f) {
                    const float arg = float(-((LD(v) * v * v * 95000.0 + 5000.0) * invSr_));
                    damp_ = float(1.0 - std::exp(LD(arg)));
                }
                return;
            case 7:
                target_ = float((1.0 - v) - v);
                depthOfs_ = v <= 0.498f ? v + v : 1.f;
                return;
            case 8: thr_ = v < 0.01f ? 0.03f : float(LD(v) * v * v); return;
            case 9: att_ = float(1.0 - std::exp(-(std::exp(5.0 - double(v) * 4.0) * double(invSr_)))); return;
            case 10: rel_ = float(1.0 - std::exp(-(std::exp(4.0 - double(v) * 4.0) * double(invSr_)))); return;
            default: return;
        }
        // x87: the time factor is stored with fst, but the extended value scales the length
        LD t;
        if (p_[1] <= 0.5f) {
            t = (LD(p_[2]) * p_[2] * LD(0.999f) + LD(0.001f)) * LD(L_ - 1);
        } else {  // tempo sync: samples-per-beat / note value, halved until it fits
            t = LD(beat_) / LD(noteValue(1.f - p_[2]));
            while (LD(L_ - 1) < t) t *= 0.5;
        }
        if (p_[4] <= 0.5f) {
            tapR_ = int(t);
            tapL_ = int((LD(p_[4]) + 0.5) * t);
        } else {
            tapL_ = int(t);
            tapR_ = int((1.5 - LD(p_[4])) * t);
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
    const float* pitch_ = nullptr;
    int L_ = 0, w_ = 0, tapL_ = 0, tapR_ = 0;
    float s_ = 0, lo_ = 0, env_ = 0, mix_ = 0.5f, fbk_ = 0, damp_ = 1, beat_ = 0.f;
    float loAmt_ = 0, loC_ = 0, target_ = 1, depthOfs_ = 0, thr_ = 0, att_ = 0, rel_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_14() { return std::make_unique<DuckingDelay>(); }

}  // namespace hs2
