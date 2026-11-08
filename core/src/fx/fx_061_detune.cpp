// 61 Detune (ctor FUN_10038590, vtable 0x100a9554: setSampleRate FUN_100385e0, process FUN_10038700,
// setParam FUN_10038a90; init/tempo 0x10002170 is a no-op). Shares setSampleRate/process with 60 Pitch Shift.
// Two-tap crossfading pitch shifter per channel on a 1.2 s line: a read phase in [0, N) advances by `rate` per
// sample (taps at phase and phase + N, crossfade x = (1 - phase/N)^2, weight 2x - x^2). L is shifted up and R down
// by p2^2 * 50 cents (rate = e^(p2^2 * ln2/24) - 1 and its reciprocal - 1). p0 mix (wet 2m-m^2, dry 1-m^2),
// p3 window N = (0.005 + 0.035 p3) * sr. Detune has no feedback (0x84 = 0), routing 1 and no extra delay.
// The emulated x87 runs at 53-bit precision (MSVC default), so "extended" maths is done in double here.
// Oracle-verified: exact (-240 dB, i.e. bit-identical) over --random 8 and p = all 0 / all 1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class Detune : public EffectBase {
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
    void process(float* io, int n) override {
        if (bufR_.empty() || L_ <= 0) return;
        const int L1 = L_ - 1;
        const float invN = float(1.0 / double(N_));
        const double mm = double(mix_) * double(mix_);
        const float dry = float(1.0 - mm), wet = float((double(mix_) + double(mix_)) - mm);
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            if (--w_ < 0) {
                w_ = L1;
                bufL_[L_] = bufL_[0];
                bufR_[L_] = bufR_[0];
            }
            // no feedback in Detune (0x84 = 0), routing 1: L from L, R from R
            bufL_[w_] = float(double(yL_) * 0.0 + double(inL));
            bufR_[w_] = float(double(yR_) * 0.0 + double(inR));
            double y = tap(bufL_, phL_, rateL_, invN);
            yL_ = float(y);
            io[2 * i] = float(y * double(wet) + double(inL) * double(dry));
            y = tap(bufR_, phR_, rateR_, invN);
            yR_ = float(y);
            io[2 * i + 1] = float(y * double(wet) + double(inR) * double(dry));
        }
    }

private:
    double tap(const std::vector<float>& b, float& ph, float rate, float invN) const {
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
        int idx = ip + w_;  // + extra delay 0
        if (L_ - 1 < idx) idx -= L_;
        double a = b[idx];
        const double y1 = (double(b[idx + 1]) - a) * fr + a;
        idx += N_;
        if (L_ - 1 < idx) idx -= L_;
        a = b[idx];
        const double y2 = (double(b[idx + 1]) - a) * fr + a;
        return y1 + (y2 - y1) * g;
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: {
                // 2^(log2e * v*v * ln2/24) computed on the x87 (f2xm1/fscale)
                const double e = std::exp2((double(v) * double(v) * double(0.028881133f)) * 1.4426950408889634);
                rateL_ = float(e - 1.0);
                rateR_ = float(1.0 / e - 1.0);
                return;
            }
            case 3: N_ = int((double(v) * double(0.035f) + double(0.005f)) * double(sr_)); return;
            default: return;
        }
    }
    std::vector<float> bufL_, bufR_;
    int L_ = 0, w_ = 0, N_ = 0;
    float mix_ = 0.5f, rateL_ = 0, rateR_ = 0, phL_ = 0, phR_ = 0, yL_ = 0, yR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_61() { return std::make_unique<Detune>(); }

}  // namespace hs2
