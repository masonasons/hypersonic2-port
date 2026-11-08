// 54 Scanner Vibrato (ctor FUN_10035ea0, vtable 0x100a9388: setSampleRate FUN_10036000, process FUN_10036100,
// setParam FUN_10036470; init/tempo 0x10002170 is a no-op).
// Hammond-style scanner: a 10 ms interleaved stereo line read by a tap that steps through 16 scanner positions
// (0..8..1, folded) per LFO cycle; each step moves to delay table[mode][k] * depth samples and the read crossfades
// from the previous tap with a smoothstep (3-2x)x^2 of x = phase * (7 p4^2 + 1). Amplitude 1 - table*0.054 p5
// also crossfades. p0 mode = -3 - trunc(-6.9 p0): -3..-1 vibrato V3..V1 (wet gain 1.3), 0 bypass (in place,
// untouched), 1..3 chorus C1..C3 (wet gain 0.08 mode + 0.45, plus 1.875 (dry - 0.68 * one-pole LP of dry)).
// p2 rate (6.86667 e^(1.386 p2 - 0.693) Hz, 16 steps per cycle), p3 depth (p3 * sr * 0.000124 samples per unit),
// p5 amplitude modulation (default 0.5). Taps whose |value| > 10 are muted. Table row 3 is an original quirk
// (0, 3.4, 3, 7.8, 9.6, 12, 13, 14, 15). The current gain stays x87-extended inside a block and is rounded to
// float at the end (mirrored). The emulated x87 runs at 53-bit precision, so "extended" maths is double here.
// Oracle-verified: bit-exact (-240 dB) over --random 8 and all 0 / all 1 / every mode.
#include "effect_base.h"

namespace hs2 {
namespace {

class ScannerVibrato : public EffectBase {
public:
    ScannerVibrato() { p_[5] = 0.5f; }
    void setSampleRate(float sr) override {
        sr_ = sr;
        const double inv = 1.0 / double(sr);
        invSr_ = float(inv);
        const int half = int(double(sr) * double(0.01f));
        gateLen_ = half;  // +0xc tail = the line length (FUN_10036000)
        lpk_ = float(inv * 8800.0);
        w_ = idxA_ = idxB_ = step_ = 1;
        ph_ = 0.f;
        lpL_ = lpR_ = 0.f;
        gCur_ = gPrev_ = 0.f;
        L_ = half * 2;  // interleaved L/R pairs
        buf_.assign(size_t(L_) + 2, 0.f);
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // Gate (FUN_10036100): the bypass check (mode 0) comes before the silence gate, so in bypass the flag stays set.
    void processGated(float* lr, int n, bool& silent) override {
        if (buf_.empty() || L_ <= 0 || mode_ == 0) {
            lpL_ = lpR_ = 0.f;
            return;
        }
        Effect::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        if (buf_.empty() || L_ <= 0 || mode_ == 0) {
            lpL_ = lpR_ = 0.f;
            return;
        }
        static const float kTab[3][9] = {{0, 1, 2, 3, 4, 5, 6, 7, 8},
                                         {0, 1, 2, 4, 6, 8, 9, 10, 12},
                                         {0, 3.4f, 3, 7.8f, 9.6f, 12, 13, 14, 15}};
        const int L1 = L_ - 1;
        float dryK = 0.f, gain = 1.3f;
        if (mode_ > 0) {
            dryK = 1.875f;
            gain = float(double(mode_) * double(0.08f) + double(0.45f));
        }
        double gCur = gCur_;
        int w = w_, a = idxA_, b = idxB_;
        for (int i = 0; i < n; ++i) {
            const double ph = double(ph_) + double(rate_);
            ph_ = float(ph);
            if (ph > 1.0) {
                ph_ = float(ph - 1.0);
                if (++step_ == 16) step_ = 0;
                b = a;
                gPrev_ = float(gCur);
                const int k = step_ > 8 ? 16 - step_ : step_;
                const int m = mode_ < 0 ? -mode_ : mode_;
                const double T = kTab[m - 1][k];
                a = w + 2 * int(T * double(depth_));
                gCur = 1.0 - T * double(am_);
                if (L1 < a) a -= L_;
            }
            double c = double(xf_) * double(ph_);
            c = c > 1.0 ? 1.0 : ((3.0 - (c + c)) * c) * c;
            const float G = float(((gCur - double(gPrev_)) * c + double(gPrev_)) * double(gain));
            const float inL = io[2 * i], inR = io[2 * i + 1];
            w -= 2;
            buf_[w + 2] = inL;
            buf_[w + 1] = inR;
            if (w < 0) w = L1;
            double t = buf_[b];
            double y = (double(buf_[a]) - t) * c + t;
            lpL_ = float(((double(inL) + double(1e-8f)) - double(lpL_)) * double(lpk_) + double(lpL_));
            if (std::fabs(y) > 10.0) y = 0.0;
            io[2 * i] = float(y * double(G) + (double(inL) - double(lpL_) * double(0.68f)) * double(dryK));
            t = buf_[b - 1];
            y = t + (double(buf_[a - 1]) - t) * c;
            lpR_ = float(((double(inR) + double(1e-8f)) - double(lpR_)) * double(lpk_) + double(lpR_));
            if (std::fabs(y) > 10.0) y = 0.0;
            io[2 * i + 1] = float(y * double(G) + (double(inR) - double(lpR_) * double(0.68f)) * double(dryK));
            a -= 2;
            b -= 2;
            if (a < 0) a = L1;
            if (b < 0) b = L1;
        }
        gCur_ = float(gCur);
        w_ = w;
        idxA_ = a;
        idxB_ = b;
        if (std::fabs(lpL_) < 1e-8f) lpL_ = 0.f;
        if (std::fabs(lpR_) < 1e-8f) lpR_ = 0.f;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mode_ = -3 - int(double(v) * double(-6.9f)); return;
            case 2: {
                const double hz =
                    std::exp2((double(v) * double(1.386f) - double(0.693f)) * 1.4426950408889634) * double(6.86667f);
                rate_ = float(hz * double(invSr_) * 16.0);
                return;
            }
            case 3: depth_ = float(double(v) * double(sr_) * double(0.000124f)); return;
            case 4: xf_ = float(double(v) * double(v) * 7.0 + 1.0); return;
            case 5: am_ = float(double(v) * double(0.054f)); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 1, idxA_ = 1, idxB_ = 1, step_ = 1, mode_ = 0;
    float ph_ = 0, rate_ = 0, depth_ = 0, xf_ = 1, am_ = 0, lpk_ = 0;
    float gCur_ = 0, gPrev_ = 0, lpL_ = 0, lpR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_54() { return std::make_unique<ScannerVibrato>(); }

}  // namespace hs2
