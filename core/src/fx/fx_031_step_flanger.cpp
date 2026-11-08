// 31 Step Flanger (ctor FUN_10033300, setSampleRate FUN_10033340, setParam FUN_100338d0, process FUN_10033480,
// init/tempo FUN_10033460; vtable 0x100a9230).
// Flanger-style interleaved stereo line of 2 * int(0.012 sr) floats (R at even, L at odd indices; written with
// in + feedback * previous tap + 1e-8). Every 64 samples a triangle LFO (rate p2) gives a delay
// T = |(tri - 1) * depth + centre| (tri in [0, 2]); the smooth read position glides to T - T * step. A second
// "step" phase (rate p6, or tempo-synced when p1 > 0.5) wraps every 1.0; on each wrap the extra offset
// 2 * int(T * step) is re-sampled and the old offset is cross-faded out over 64 samples. The read tap is
// position + offset. Out = in * dry - tap * wet. p0 mix (dry 1-m^2, wet (2m-m^2)/2), p1 sync, p2 LFO rate,
// p3 depth (0.004 sr p3), p4 feedback (-0.95 p4), p5 centre (0.004 sr p5), p6 step rate / note value,
// p7 step amount (2 p7 - p7^2). Ctor defaults p4 = 0.75, p5 = 0.5.
// Sync mode: at the start of every process() call the original resets the step phase to
// fmod(noteValue * songPos, 1) (+1 if the old phase minus that exceeds 0.9), songPos being the double passed in
// process()'s 3rd/4th argument (Effect::setSongPos; 0 unless the part sets it, which is what the oracle
// passes). The tempo method (+0xc, FUN_10033460) only stores beat * 16 at +0xe0, which nothing reads, so it is not
// overridden.
// Oracle-verified: bit-exact (-240 dB) on --random 8 plus all-0, all-1 and hand-picked sets (sync on and off).
#include "effect_base.h"

namespace hs2 {
namespace {

// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale (53-bit precision control).
double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}

class StepFlanger : public EffectBase {
public:
    StepFlanger() {
        p_[4] = 0.75f;
        p_[5] = 0.5f;
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.012f)) * 2;
        buf_.assign(size_t(L_) + 4, 0.f);
        w_ = r_ = 1;
        count_ = 1;
        yL_ = yR_ = vel_ = frac_ = ph_ = stepPh_ = 0;
        off_ = oldOff_ = 0;
        fade_ = fadeStep_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const double m2 = double(mix_) * double(mix_);
        const float dry = float(1.0 - m2), wet = float(((double(mix_) + double(mix_)) - m2) * 0.5);
        const int L1 = L_ - 1;
        if (sync_) {
            double q = std::fmod(double(note_) * songPos_, 1.0);
            if (double(0.9f) < double(stepPh_) - q) q += 1.0;
            stepPh_ = float(q);
        }
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                double s = double(inc2_) + double(ph_);
                ph_ = float(s);
                if (4.0 < s) ph_ = float(s - 4.0);
                const double tri = ph_ <= 2.f ? double(ph_) : 4.0 - double(ph_);
                double t = (tri - 1.0) * double(depth_) + double(centre_);
                if (t < 0.0) t = -t;
                s = double(stepInc_) + double(stepPh_);
                stepPh_ = float(s);
                if (1.0 < s) {
                    oldOff_ = off_;
                    stepPh_ = float(s - 1.0);
                    fade_ = 1.f;
                    fadeStep_ = 0.015625f;
                    off_ = int(t * double(step_)) * 2;
                }
                int d = r_ - w_;
                if (d < 0) d += L_;
                const double v = (((t - t * double(step_)) - double(frac_)) - double(d >> 1)) * 0.015625 - 1.0;
                vel_ = v > 0.0 ? 0.f : float(v);
                count_ = 64;
            }
            const float inL = io[2 * i], inR = io[2 * i + 1];
            w_ -= 2;
            b[w_ + 2] = float((double(fb_) * double(yL_) + double(inL)) + double(1e-8f));
            b[w_ + 1] = float((double(yR_) * double(fb_) + double(inR)) + double(1e-8f));
            if (w_ < 0) {
                b[L_] = b[0];
                b[L_ + 1] = b[1];
                w_ = L1;
            }
            int k = off_ + r_;
            if (L1 < k) k -= L_;
            yL_ = float(interp(b, k + 2, k));
            yR_ = float(interp(b, k + 1, k - 1));
            if (fade_ > 0.f) {
                const double f = double(fade_) - double(fadeStep_);
                fade_ = float(f);
                if (f < 0.0) fade_ = fadeStep_ = 0.f;
                k = oldOff_ + r_;
                if (L1 < k) k -= L_;
                yL_ = float((interp(b, k + 2, k) - double(yL_)) * double(fade_) + double(yL_));
                yR_ = float((interp(b, k + 1, k - 1) - double(yR_)) * double(fade_) + double(yR_));
            }
            io[2 * i] = float(double(inL) * dry - double(wet) * double(yL_));
            io[2 * i + 1] = float(double(inR) * dry - double(wet) * double(yR_));
            const double s = double(frac_) + double(vel_);
            frac_ = float(s);
            if (s < 0.0) {
                do {
                    r_ -= 2;
                    if (r_ < 0) r_ = L1;
                    frac_ = float(double(frac_) + 1.0);
                } while (frac_ < 0.f);
            }
        }
    }

private:
    double interp(const float* b, int hi, int lo) const {
        return (double(b[hi]) - double(b[lo])) * double(frac_) + double(b[lo]);
    }
    // FUN_100489b0: note length in beats, index trunc(v*20.9) (as in fx_010_delay.cpp)
    static float noteValue(float v) {
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 6: {
                sync_ = 0.5f < p_[1];
                note_ = noteValue(float(double(p_[6]) * 0.75 + 0.25));
                const double r = x87exp(double(p_[6]) * double(5.991464f) - double(2.995732f));
                stepInc_ = float(r * double(invSr_) * 64.0);
                return;
            }
            case 2: {
                const double r = x87exp(double(p_[2]) * double(5.991464f) - double(2.995732f));
                inc2_ = float(r * double(invSr_) * 256.0);
                return;
            }
            case 3: depth_ = float(double(v) * double(sr_) * double(0.004f)); return;
            case 4: fb_ = float(double(v) * double(-0.95f)); return;
            case 5: centre_ = float(double(v) * double(sr_) * double(0.004f)); return;
            case 7: step_ = float((double(v) + double(v)) - double(v) * double(v)); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 1, r_ = 1, count_ = 1, off_ = 0, oldOff_ = 0;
    bool sync_ = false;
    float mix_ = 0.5f, note_ = 1.f, stepInc_ = 0, inc2_ = 0, depth_ = 0, fb_ = 0, centre_ = 0, step_ = 0;
    float yL_ = 0, yR_ = 0, vel_ = 0, frac_ = 0, ph_ = 0, stepPh_ = 0, fade_ = 0, fadeStep_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_31() { return std::make_unique<StepFlanger>(); }

}  // namespace hs2
