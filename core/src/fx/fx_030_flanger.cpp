// 30 Flanger (ctor FUN_10032d80, setSampleRate FUN_10032dc0, setParam FUN_10033140, process FUN_10032ec0;
// vtable 0x100a91f8, init/tempo 0x10002170 is a no-op).
// One interleaved stereo line of 2 * int(0.025 sr) floats (R at even, L at odd indices), written with
// in + feedback * previous tap output + 1e-8. A single fractional read position (shared by L and R) glides every
// 256 samples towards |sin(ph) * depth + centre| samples. Out = in * dry - tap * wet (the wet signal is
// subtracted). p0 mix (dry 1-m^2, wet (2m-m^2)/2), p1/p2 rate (last set wins), p3 depth (0.012 sr p3), p4 feedback
// (-0.95 p4), p5 centre (0.012 sr p5). Ctor defaults p4 = 0.75, p5 = 0.5.
// The original keeps the LFO target and the tap reads on the x87 stack (53-bit precision), mirrored with double;
// the emulated fsin is the host double sin().
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

class Flanger : public EffectBase {
public:
    Flanger() {
        p_[4] = 0.75f;
        p_[5] = 0.5f;
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.025f)) * 2;
        buf_.assign(size_t(L_) + 2, 0.f);
        w_ = r_ = 1;
        count_ = 1;
        ph_ = frac_ = vel_ = yL_ = yR_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const double m2 = double(mix_) * double(mix_);
        const float dry = float(1.0 - m2), wet = float(((double(mix_) + double(mix_)) - m2) * 0.5);
        const int L1 = L_ - 1;
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                const double s = double(inc_) + double(ph_);
                ph_ = float(s);
                if (double(6.2831855f) < s) ph_ = float(s - double(6.2831855f));
                double t = std::sin(double(ph_)) * double(depth_) + double(centre_);
                if (t < 0.0) t = -t;
                int d = r_ - w_;
                if (d < 0) d += L_;
                const double v = ((t - double(frac_)) - double(d >> 1)) * 0.00390625 - 1.0;
                vel_ = v > 0.0 ? 0.f : float(v);
                count_ = 256;
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
            const double yl = (double(b[r_ + 2]) - double(b[r_])) * double(frac_) + double(b[r_]);
            yL_ = float(yl);
            io[2 * i] = float(double(inL) * dry - yl * wet);
            const double yr = (double(b[r_ + 1]) - double(b[r_ - 1])) * double(frac_) + double(b[r_ - 1]);
            yR_ = float(yr);
            io[2 * i + 1] = float(double(inR) * dry - yr * wet);
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
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2: {
                const double r = x87exp(double(v) * double(5.991464f) - double(2.995732f));
                inc_ = float(r * double(invSr_) * double(1608.4955f));
                return;
            }
            case 3: depth_ = float(double(v) * double(sr_) * double(0.012f)); return;
            case 4: fb_ = float(double(v) * double(-0.95f)); return;
            case 5: centre_ = float(double(v) * double(sr_) * double(0.012f)); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 1, r_ = 1, count_ = 1;
    float mix_ = 0.5f, inc_ = 0, depth_ = 0, fb_ = 0, centre_ = 0;
    float ph_ = 0, frac_ = 0, vel_ = 0, yL_ = 0, yR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_30() { return std::make_unique<Flanger>(); }

}  // namespace hs2
