// 60 Pitch Shift (ctor FUN_10043870, vtable 0x100a9d60: setSampleRate FUN_100385e0, process FUN_10038700 (both
// shared with 61 Detune), setParam FUN_10038cb0, tempo FUN_10038c80 (beat = 1/b; never called by the oracle,
// so beat = 0 and a synced delay is 0).
// Two-tap crossfading pitch shifter per channel on a 1.2 s line (see fx_061_detune.cpp): read phase in [0, N)
// advancing by rate per sample, taps at phase and phase + N, crossfade x = (1 - phase/N)^2, weight 2x - x^2.
// p0 mix (wet 2m-m^2, dry 1-m^2), p2/p3 L/R shift: rate = 1 - e^(1.38629 p - 0.693147) (-12..+12 semitones),
// p4 window N = (0.005 + 0.035 p4) * sr, p1 sync / p5 delay: d = trunc((p5^2*0.999 + 0.001) * sr) (sync: beat /
// note(1-p5), halved until <= sr); extra tap delay = d - N/2 if d >= N else 0 (only recomputed on p1/p5),
// p6 feedback (-0.999 p6, the previous wet output written back into the line), p7 routing trunc(p7*4.9):
// 0 both lines fed back from L, 1 L->L R->R, 2 no feedback and no extra delay, 3 crossed, 4 both from R.
// The emulated x87 runs at 53-bit precision (MSVC default), so "extended" maths is done in double here.
// Oracle-verified: exact (-240 dB) over --random 8 and hand-picked sets (all 0, all 1, every routing, sync on).
#include "effect_base.h"

namespace hs2 {
namespace {

class PitchShift : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        w_ = 0;
        phL_ = phR_ = 0.f;
        yL_ = yR_ = 0.f;
        const int L = int(double(sr) * double(1.2f));
        if (L != L_) {
            L_ = L;
            bufL_.assign(size_t(L_) + 2, 0.f);
            bufR_.assign(size_t(L_) + 2, 0.f);
        } else {
            std::fill(bufL_.begin(), bufL_.begin() + L_, 0.f);
            std::fill(bufR_.begin(), bufR_.begin() + L_, 0.f);
        }
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_10038c80): beat = 1/b, then setParam(2, p2) - copied from Delay, so it re-applies the L pitch,
    // not the delay (an original quirk: the synced delay only updates on the next p1/p5 change). Never called by the oracle.
    void setTempo(float, float b) override {
        beat_ = float(1.0 / double(b));
        setParam(2, p_[2]);
    }
    void process(float* io, int n) override {
        if (bufR_.empty() || L_ <= 0) return;
        const int L1 = L_ - 1;
        const float invN = float(1.0 / double(N_));
        const double mm = double(mix_) * double(mix_);
        const float dry = float(1.0 - mm), wet = float((double(mix_) + double(mix_)) - mm);
        int off = off_;
        const float zero = 0.f;
        const float* srcL = &yL_;  // feedback source of the L line
        const float* srcR = &yR_;  // feedback source of the R line
        switch (routing_) {
            case 0: srcR = &yL_; break;
            case 2: yL_ = yR_ = 0.f; srcL = srcR = &zero; off = 0; break;
            case 3: srcL = &yR_; srcR = &yL_; break;
            case 4: srcL = &yR_; break;
            default: break;
        }
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            if (--w_ < 0) {
                w_ = L1;
                bufL_[L_] = bufL_[0];
                bufR_[L_] = bufR_[0];
            }
            bufL_[w_] = float(double(*srcL) * double(fb_) + double(inL));
            bufR_[w_] = float(double(*srcR) * double(fb_) + double(inR));
            double y = tap(bufL_, phL_, rateL_, invN, off);
            yL_ = float(y);
            io[2 * i] = float(y * double(wet) + double(inL) * double(dry));
            y = tap(bufR_, phR_, rateR_, invN, off);
            yR_ = float(y);
            io[2 * i + 1] = float(y * double(wet) + double(inR) * double(dry));
        }
    }

private:
    double tap(const std::vector<float>& b, float& ph, float rate, float invN, int off) const {
        const double s = double(rate) + double(ph);
        const double N = double(N_);
        if (s < 0.0) ph = float(N + s);
        else if (N <= s) ph = float(s - N);
        else ph = float(s);
        double x = 1.0 - double(invN) * double(ph);
        x = x * x;
        const double g = (x + x) - x * x;
        const int ip = int(ph);
        const double fr = double(ph) - double(ip);
        int idx = ip + w_ + off;
        if (L_ - 1 < idx) idx -= L_;
        double a = b[idx];
        const double y1 = (double(b[idx + 1]) - a) * fr + a;
        idx += N_;
        if (L_ - 1 < idx) idx -= L_;
        a = b[idx];
        const double y2 = (double(b[idx + 1]) - a) * fr + a;
        return y1 + (y2 - y1) * g;
    }
    static float semis(float v) {
        return float(1.0 - std::exp2((double(v) * double(1.38629f) - double(0.693147f)) * 1.4426950408889634));
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: rateL_ = semis(v); return;
            case 3: rateR_ = semis(v); return;
            case 4: N_ = int((double(v) * double(0.035f) + double(0.005f)) * double(sr_)); return;
            case 6: fb_ = float(double(v) * double(-0.999f)); return;
            case 7: routing_ = int(double(v) * double(4.9f)); return;
            case 1: case 5: break;
            default: return;
        }
        double t;
        if (0.5f < p_[1]) {
            t = double(beat_) / double(noteValue(1.f - p_[5]));
            while (double(sr_) < t) t *= 0.5;
        } else {
            t = (double(p_[5]) * double(p_[5]) * double(0.999f) + double(0.001f)) * double(sr_);
        }
        const int d = int(t);
        off_ = d < N_ ? 0 : d - N_ / 2;
    }
    // FUN_100489b0: note length in beats, index trunc(v*20.9) (same table as fx_010_delay.cpp)
    static float noteValue(float v) {
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    std::vector<float> bufL_, bufR_;
    int L_ = 0, w_ = 0, N_ = 0, off_ = 0, routing_ = 1;
    float mix_ = 0.5f, rateL_ = 0, rateR_ = 0, fb_ = 0, beat_ = 0;
    float phL_ = 0, phR_ = 0, yL_ = 0, yR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_60() { return std::make_unique<PitchShift>(); }

}  // namespace hs2
