// 21 Quad Chorus (ctor FUN_10031610, setSampleRate FUN_10031630, setParam FUN_10031cc0, process FUN_10031740;
// vtable 0x100a914c, init/tempo 0x10002170 is a no-op).
// Mono feedback line of 0.1 s; four fractional taps (two per side) glide every 256 samples towards targets set by
// triangle LFOs: L taps base + tri*depth and base + depth - tri*depth, R the same with a 1.41x faster LFO. The two
// taps of each side are summed. The line input is (L + R + (wetL + wetR) * fb) through a 2-pole state-variable
// filter (low-pass for p5 <= 0.5, high-pass above). p0 mix (dry 1-m^2, wet (2m-m^2)/4), p1/p2 rate (last set
// wins), p2/p3 depth ((0.048 - 0.032 p2) * sr * p3^2), p4 pre-delay, p5 filter, p6 feedback (-0.01 - 0.23 p6).
// The original keeps the wet sums, LFO targets and filter intermediates on the x87 stack (53-bit precision
// control), mirrored here with double.
// Oracle-verified: bit-exact (-240 dB) on --random 8 plus all-0, all-1 and a hand-picked set.
#include "effect_base.h"

namespace hs2 {
namespace {

// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale (53-bit precision control).
double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}

class QuadChorus : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.1f));
        buf_.assign(size_t(L_) + 2, 0.f);
        count_ = 1;
        ph_ = ph2_ = lp_ = bp_ = 0;
        std::fill(std::begin(tap_), std::end(tap_), 0);
        std::fill(std::begin(frac_), std::end(frac_), 0.f);
        std::fill(std::begin(vel_), std::end(vel_), 0.f);
        w_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const double m2 = double(mix_) * double(mix_);
        const float dry = float(1.0 - m2), wet = float(((double(mix_) + double(mix_)) - m2) * 0.25);
        const float inc2 = float(double(inc_) * double(1.41f));
        const int L1 = L_ - 1;
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                const double top = double(depth_) + double(base_);
                double s = double(inc_) + double(ph_);
                ph_ = float(s);
                if (2.0 < s) ph_ = float(s - 2.0);
                double tri = ph_ <= 1.f ? double(ph_) : 2.0 - double(ph_);
                const float t0 = float(tri * double(depth_));
                vel_[0] = velocity(double(t0) + double(base_), 0);
                vel_[1] = velocity(top - double(t0), 1);
                s = double(inc2) + double(ph2_);
                ph2_ = float(s);
                if (2.0 < s) ph2_ = float(s - 2.0);
                tri = ph2_ <= 1.f ? double(ph2_) : 2.0 - double(ph2_);
                const double t2 = tri * double(depth_);
                vel_[2] = velocity(t2 + double(base_), 2);
                vel_[3] = velocity(top - t2, 3);
                count_ = 256;
            }
            const float inL = io[2 * i], inR = io[2 * i + 1];
            const double wl = read(b, 0, 1);
            io[2 * i] = float(double(inL) * dry + wl * wet);
            advance(0, L1);
            advance(1, L1);
            const double wr = read(b, 2, 3);
            io[2 * i + 1] = float(double(inR) * dry + wr * wet);
            advance(2, L1);
            advance(3, L1);
            const float x = float((((double(inR) + double(inL)) - lp_) - bp_) + (wr + wl) * double(fb_));
            const double bp = double(x) * double(g_) + double(bp_);
            bp_ = float(bp);
            lp_ = float(bp * double(g_) + double(lp_));
            b[w_] = highpass_ ? x : lp_;
            if (--w_ < 0) {
                w_ = L1;
                b[L_] = b[0];
            }
        }
    }

private:
    float velocity(double target, int k) const {
        int d = tap_[k] - w_;
        if (d < 0) d += L_;
        double v = ((target - double(frac_[k])) - double(d)) * 0.00390625 - 1.0;
        return v > 0.0 ? 0.f : float(v);
    }
    double read(const float* b, int k0, int k1) const {
        const float a = b[tap_[k0]];
        double s = (double(b[tap_[k0] + 1]) - a) * frac_[k0] + a;
        s += (double(b[tap_[k1] + 1]) - b[tap_[k1]]) * frac_[k1];
        return s + b[tap_[k1]];
    }
    void advance(int k, int L1) {
        const double s = double(vel_[k]) + double(frac_[k]);
        frac_[k] = float(s);
        if (s < 0.0) {
            do {
                if (--tap_[k] < 0) tap_[k] = L1;
                frac_[k] = float(double(frac_[k]) + 1.0);
            } while (frac_[k] < 0.f);
        }
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2: {
                const double r = x87exp(double(v) * double(5.991464f) - double(2.995732f));
                rate_ = float(r);
                inc_ = float(r * double(invSr_) * double(108.8f));
            }
                [[fallthrough]];
            case 3:
                depth_ = float((double(0.048f) - double(p_[2]) * double(0.032f)) * double(sr_) * double(p_[3]) *
                               double(p_[3]));
                return;
            case 4: base_ = float(double(v) * double(sr_) * double(0.024f) + 1.0); return;
            case 5:
                if (v <= 0.5f) {
                    highpass_ = false;
                    g_ = float((double(v) * double(v) * 160000.0 + 4000.0) * double(invSr_));
                } else {
                    highpass_ = true;
                    const double d = double(v) - 0.5;
                    g_ = float((d * d * 32000.0 + 50.0) * double(invSr_));
                }
                return;
            case 6: fb_ = float(double(-0.01f) - double(v) * double(0.23f)); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, count_ = 1, tap_[4] = {};
    bool highpass_ = false;
    float mix_ = 0.5f, rate_ = 1.f, inc_ = 0, depth_ = 0, base_ = 1, g_ = 0, fb_ = 0;
    float ph_ = 0, ph2_ = 0, lp_ = 0, bp_ = 0, frac_[4] = {}, vel_[4] = {};
};

}  // namespace

std::unique_ptr<Effect> make_fx_21() { return std::make_unique<QuadChorus>(); }

}  // namespace hs2
