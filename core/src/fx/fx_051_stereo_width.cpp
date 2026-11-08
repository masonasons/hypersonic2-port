// 51 Stereo Width (ctor FUN_10034090, vtable 0x100a92a8: setSampleRate FUN_100340b0, process FUN_100341b0,
// setParam FUN_100346d0; init/tempo 0x10002170 is a no-op).
// Mid/side widener: side (L-R) or mid (L+R) goes through a 20 ms delay (p2^2 * 16 ms), is split into three bands
// by two one-poles (15 kHz and 2 kHz: lows = lp2, mids = lp1 - lp2, highs = x - lp1), weighted by p4 / p5 / p6
// times 4 p3, and the result s is added to / subtracted from the mid: L = (mid + s) g, R = (mid - s) g with
// g = e^(2.77588 p0 - 2.07944) (0.125..2). p7 mode trunc(3.9 p7): 0 side, 1 side inverted, 2 mid (Haas-like),
// 3 mid with L = mid g, R = s g. The emulated x87 runs at 53-bit precision, so "extended" maths is double here.
// Oracle-verified: bit-exact (-240 dB) over --random 8 and all 0 / all 1 / every mode.
#include "effect_base.h"

namespace hs2 {
namespace {

class StereoWidth : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.05);  // +0xc tail: trunc(sr * 0.05)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        w_ = 0;
        lp1_ = lp2_ = 0.f;
        const int L = int(double(sr) * double(0.02f));
        if (L != L_) {
            L_ = L;
            buf_.assign(size_t(std::max(L_, 0)), 0.f);
        } else {
            std::fill(buf_.begin(), buf_.end(), 0.f);
        }
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        if (buf_.empty()) return;
        const int L1 = L_ - 1;
        int r = delay_ + w_;
        if (L1 < r) r -= L_;
        float c = amt_;
        if (mode_ == 1) c = -c;
        const float hi = float(double(c) * double(hiW_));
        const float mid = float(double(c) * double(midW_));
        const float lo = float(double(c) * double(loW_));
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            const float S = float(double(inL) - double(inR));
            const float M = float(double(inL) + double(inR));
            --w_;
            buf_[w_ + 1] = float(double(mode_ > 1 ? M : S) + double(1e-8f));
            if (w_ < 0) w_ = L1;
            --r;
            const double x = buf_[r + 1];
            if (r < 0) r = L1;
            const double a = (x - double(lp1_)) * double(k1_) + double(lp1_);
            lp1_ = float(a);
            const double b = (a - double(lp2_)) * double(k2_) + double(lp2_);
            lp2_ = float(b);
            const double s = ((x - a) * double(hi) + (a - b) * double(mid)) + b * double(lo);
            if (mode_ == 3) {
                io[2 * i] = float(double(M) * double(gain_));
                io[2 * i + 1] = float(s * double(gain_));
            } else {
                io[2 * i] = float((double(M) + s) * double(gain_));
                io[2 * i + 1] = float((double(M) - s) * double(gain_));
            }
        }
    }

private:
    static float expf87(double x) { return float(std::exp2(x * 1.4426950408889634)); }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: gain_ = expf87(double(v) * double(2.77588f) - double(2.07944f)); return;
            case 2: delay_ = int(double(v) * double(sr_) * double(v) * double(0.016f)); return;
            case 3: amt_ = float(double(v) * 4.0); return;
            case 4:
                loW_ = v;
                k2_ = float(1.0 - std::exp2(double(invSr_) * -2000.0 * 1.4426950408889634));
                return;
            case 5:
                midW_ = v;
                k1_ = float(1.0 - std::exp2(double(invSr_) * -15000.0 * 1.4426950408889634));
                return;
            case 6: hiW_ = v; return;
            case 7: mode_ = int(double(v) * double(3.9f)); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, delay_ = 0, mode_ = 0;
    float gain_ = 1, amt_ = 0, loW_ = 0, midW_ = 0, hiW_ = 0, k1_ = 0, k2_ = 0, lp1_ = 0, lp2_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_51() { return std::make_unique<StereoWidth>(); }

}  // namespace hs2
