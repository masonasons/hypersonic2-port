// 23 Ensemble (ctor FUN_10032660, setSampleRate FUN_10032680, setParam FUN_10032bb0, process FUN_10032770;
// vtable 0x100a91c4, init/tempo 0x10002170 is a no-op).
// Mono-sum 50 ms line with three fractional taps. Every 32 samples one LFO phase (sine, 2 pi wrap) gives each tap a
// target delay (sin(ph + k) + (sin(n (ph + k)) + 1) * p4^2/4 + c) * depth for (k, n, c) = (0, 9, 1.5), (2, 8, 1.5),
// (4, 7, 1) and a read velocity gliding it there in 32 samples. Output L = sum + (t1 - t2) * spread,
// R = sum - (t1 - t2) * spread. p0 mix (dry 1-m^2, wet (2m-m^2) * 0.2), p1/p2 rate (last set wins), p3 depth
// (min(0.02 / rate, 0.024) * sr * p3^2 / 4), p4 modulation shimmer, p5 stereo spread (1.2 p5).
// The LFO targets and the third tap are kept on the x87 stack (53-bit precision), mirrored with double; the
// emulated fsin is the host double sin().
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

class Ensemble : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.05f));
        buf_.assign(size_t(L_) + 2, 0.f);
        w_ = 0;
        count_ = 1;
        ph_ = 0;
        for (int k = 0; k < 3; ++k) tap_[k] = 0, frac_[k] = vel_[k] = 0.f;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const double m2 = double(mix_) * double(mix_);
        const float dry = float(1.0 - m2), wet = float(((double(mix_) + double(mix_)) - m2) * double(0.2f));
        const int L1 = L_ - 1;
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                const double s = double(inc_) + double(ph_);
                ph_ = float(s);
                if (double(6.2831855f) < s) ph_ = float(s - double(6.2831855f));
                const double p = ph_;
                target(0, std::sin(p) + (std::sin(p * 9.0) + 1.0) * double(shimmer_) + 1.5);
                const double p2 = p + 2.0;
                target(1, std::sin(p2) + (std::sin(8.0 * p2) + 1.0) * double(shimmer_) + 1.5);
                const double p4 = p + 4.0;
                target(2, std::sin(p4) + (std::sin(7.0 * p4) + 1.0) * double(shimmer_) + 1.0);
                count_ = 32;
            }
            const float inL = io[2 * i], inR = io[2 * i + 1];
            --w_;
            b[w_ + 1] = float(double(inR) + double(inL));
            if (w_ < 0) {
                w_ = L1;
                b[L_] = b[0];
            }
            const float t1 = float(interp(b, 0));
            advance(0, L1);
            const float t2 = float(interp(b, 1));
            advance(1, L1);
            const double t3 = interp(b, 2);
            advance(2, L1);
            const double sum = (t3 + t2) + t1;
            const double d = (double(t1) - double(t2)) * double(spread_);
            io[2 * i] = float((sum + d) * wet + double(inL) * dry);
            io[2 * i + 1] = float((sum - d) * wet + double(inR) * dry);
        }
    }

private:
    void target(int k, double lfo) {
        int d = tap_[k] - w_;
        if (d < 0) d += L_;
        const double v = ((lfo * double(depth_) - double(frac_[k])) - double(d)) * 0.03125 - 1.0;
        vel_[k] = v > 0.0 ? 0.f : float(v);
    }
    double interp(const float* b, int k) const {
        const double a = b[tap_[k]];
        return (double(b[tap_[k] + 1]) - a) * double(frac_[k]) + a;
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
                inc_ = float(r * double(invSr_) * double(201.06194f) * double(0.842152f));
            }
                [[fallthrough]];
            case 3: {
                double f = double(0.02f) / double(rate_);
                if (double(0.024f) < f) f = double(0.024f);
                depth_ = float(f * double(sr_) * double(p_[3]) * double(p_[3]) * 0.25);
                return;
            }
            case 4: shimmer_ = float(double(v) * double(v) * 0.25); return;
            case 5: spread_ = float(double(v) * double(1.2f)); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, count_ = 1, tap_[3] = {};
    float mix_ = 0.5f, rate_ = 1.f, inc_ = 0, depth_ = 0, shimmer_ = 0, spread_ = 0, ph_ = 0;
    float frac_[3] = {}, vel_[3] = {};
};

}  // namespace

std::unique_ptr<Effect> make_fx_23() { return std::make_unique<Ensemble>(); }

}  // namespace hs2
