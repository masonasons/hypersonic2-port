// 22 Space Chorus (ctor FUN_10031f80, setSampleRate FUN_10031fb0, setParam FUN_10032490, process FUN_100320f0;
// vtable 0x100a9190, init/tempo 0x10002170 is a no-op).
// Two 0.1 s lines (one per channel). Each channel's input goes through a 2-pole state-variable filter; the
// high-pass part is written to that channel's line, the low-pass part is added to that channel's wet signal.
// Tap A reads the L line at base + tri * depth, tap B the R line at base + depth - tri * depth (one triangle LFO);
// every 256 samples each tap gets a read velocity that glides it to its target. Out L = inL * dry +
// (B - A + lpL) * m, out R = inR * dry + (A - B + lpR) * m. p0 mix (dry 1-m^2, wet m), p1/p2 rate (last set wins),
// p2/p3 depth ((0.048 - 0.032 p2) * sr * p3^2), p4 pre-delay, p5 crossover ((p5^2 * 10000 + 50) / sr).
// The original keeps the tap reads, LFO target and filter intermediates on the x87 stack (53-bit precision),
// mirrored with double.
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

class SpaceChorus : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.1f));
        bufL_.assign(size_t(L_) + 2, 0.f);
        bufR_.assign(size_t(L_) + 2, 0.f);
        w_ = iA_ = iB_ = 0;
        count_ = 1;
        ph_ = fracA_ = fracB_ = velA_ = velB_ = 0;
        bpL_ = lpL_ = bpR_ = lpR_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const float dry = float(1.0 - double(mix_) * double(mix_)), wet = mix_;
        const int L1 = L_ - 1;
        float* bl = bufL_.data();
        float* br = bufR_.data();
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                const double s = double(inc_) + double(ph_);
                ph_ = float(s);
                if (2.0 < s) ph_ = float(s - 2.0);
                const double tri = ph_ <= 1.f ? double(ph_) : 2.0 - double(ph_);
                const double t = tri * double(depth_);
                int d = iA_ - w_;
                if (d < 0) d += L_;
                double v = (((t + double(base_)) - double(fracA_)) - double(d)) * 0.00390625 - 1.0;
                velA_ = v > 0.0 ? 0.f : float(v);
                d = iB_ - w_;
                if (d < 0) d += L_;
                v = (((double(base_) + double(depth_) - t) - double(fracB_)) - double(d)) * 0.00390625 - 1.0;
                velB_ = v > 0.0 ? 0.f : float(v);
                count_ = 256;
            }
            const double a = (double(bl[iA_ + 1]) - double(bl[iA_])) * double(fracA_) + double(bl[iA_]);
            advance(iA_, fracA_, velA_, L1);
            const double b = (double(br[iB_ + 1]) - double(br[iB_])) * double(fracB_) + double(br[iB_]);
            advance(iB_, fracB_, velB_, L1);
            --w_;
            const float inL = io[2 * i];
            bl[w_ + 1] = filter(inL, bpL_, lpL_);
            io[2 * i] = float(((b - a) + double(lpL_)) * double(wet) + double(inL) * double(dry));
            const float inR = io[2 * i + 1];
            br[w_ + 1] = filter(inR, bpR_, lpR_);
            io[2 * i + 1] = float(((a - b) + double(lpR_)) * double(wet) + double(inR) * double(dry));
            if (w_ < 0) {
                bl[L_] = bl[0];
                br[L_] = br[0];
                w_ = L1;
            }
        }
    }

private:
    // state-variable filter; returns the high-pass output (as written to the line)
    float filter(float in, float& bp, float& lp) const {
        const double x = (double(in) - double(lp)) - double(bp);
        const double nb = x * double(g_) + double(bp);
        bp = float(nb);
        lp = float(nb * double(g_) + double(lp));
        return float(x);
    }
    static void advance(int& idx, float& frac, float vel, int L1) {
        const double s = double(frac) + double(vel);
        frac = float(s);
        if (s < 0.0) {
            do {
                if (--idx < 0) idx = L1;
                frac = float(double(frac) + 1.0);
            } while (frac < 0.f);
        }
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2: {
                const double r = x87exp(double(v) * double(5.991464f) - double(2.995732f));
                inc_ = float(r * double(invSr_) * double(116.352f));
            }
                [[fallthrough]];
            case 3:
                depth_ = float((double(0.048f) - double(p_[2]) * double(0.032f)) * double(sr_) * double(p_[3]) *
                               double(p_[3]));
                return;
            case 4: base_ = float(double(v) * double(sr_) * double(0.024f) + 1.0); return;
            case 5: g_ = float((double(v) * double(v) * 10000.0 + 50.0) * double(invSr_)); return;
            default: return;
        }
    }
    std::vector<float> bufL_, bufR_;
    int L_ = 0, w_ = 0, iA_ = 0, iB_ = 0, count_ = 1;
    float mix_ = 0.5f, inc_ = 0, depth_ = 0, base_ = 1, g_ = 0;
    float ph_ = 0, fracA_ = 0, fracB_ = 0, velA_ = 0, velB_ = 0, bpL_ = 0, lpL_ = 0, bpR_ = 0, lpR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_22() { return std::make_unique<SpaceChorus>(); }

}  // namespace hs2
