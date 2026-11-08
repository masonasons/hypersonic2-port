// 1 Hall Reverb (ctor FUN_100261f0, setSampleRate FUN_10026260, process FUN_10026590, setParam FUN_10026bc0).
// Predelay -> input lowpass -> low cut -> 4 series allpasses -> 6 damped combs. Transcribed literally.
// Oracle-verified: bit-exact (precision mirrored from the disassembly).
#include "effect_base.h"

namespace hs2 {
namespace {

// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale (53-bit precision control).
double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}

class HallReverb : public EffectBase {
public:
    HallReverb() {
        p_[3] = 0.5f;
        p_[4] = 0.75f;  // in_ECX[0x15], in_ECX[0x16]
    }
    void setSampleRate(float sr) override {
        gate_ = Gate::Detect;  // keeps running while its output is audible (FUN_10025480)
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.1f));
        pre_.assign(size_t(L_) * 4, 0.f);
        ap_.assign(L_, 0.f);
        for (auto& c : comb_) c.assign(L_, 0.f);
        w_ = preW_ = 0;
        lp_ = hp_ = 0;
        for (auto& d : damp_) d = 0;
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        // Precision follows the disassembly (53-bit x87): register values are double, memory stores are float.
        const int L1 = L_ - 1;
        const double D = D_;
        const double A = D * double(0.383f);
        auto wrap = [&](int x) { return L1 < x ? x - L_ : x; };
        int w = w_;
        int t0 = wrap(w + int(A * double(0.3747f)));
        int t1 = wrap(t0 + int(A * double(0.2823f)));
        int t2 = wrap(t1 + int(A * double(0.7309f)));
        int t3 = wrap(t2 + int(A));
        double f = D;
        int c0 = wrap(w + int(f));
        f = D * double(0.9231f);
        int c1 = wrap(w + int(f));
        f *= double(0.9444f);
        int c2 = wrap(w + int(f));
        f *= double(0.8971f);
        int c3 = wrap(w + int(f));
        f *= double(0.918f);
        int c4 = wrap(w + int(f));
        int c5 = wrap(w + int(f * double(0.8929f)));
        const int P1 = L_ * 4 - 1;
        int pw = preW_;
        int pr = predelay_ + pw;
        if (P1 < pr) pr -= P1;
        int apw = w;
        const float fb = fb_;
        const double k833 = double(0.833f), k65 = double(0.65f);
        for (int i = 0; i < n; ++i) {
            const double inL = io[2 * i], inR = io[2 * i + 1];
            --pw;
            pre_[pw + 1] = float(inL + inR + double(1e-8f));
            if (pw < 0) pw = P1;
            lp_ = float((double(pre_[pr]) * double(0.2f) - lp_) * lpCoef_ + lp_);
            if (--pr < 0) pr = P1;
            const double x = double(lp_) - hp_;
            hp_ = float(x * hpCoef_ + hp_);
            const double a1 = double(ap_[t0]) - k833 * x;
            const float a1F = float(a1);
            const double s1 = a1 * k833;
            const float s1F = float(s1);
            --apw;
            ap_[apw + 1] = float(s1 + x);
            if (apw < 0) apw = L1;
            const double s2 = double(ap_[t1]) - s1F;
            const float s2F = float(s2);
            --t0;
            ap_[t0 + 1] = float(s2 * k833 + a1F);
            if (t0 < 0) t0 = L1;
            --t1;
            const double a3 = double(ap_[t2]) - double(s2F) * k65;
            const float a3F = float(a3);
            const double y1 = a3 * k65;
            ap_[t1 + 1] = float(double(s2F) + y1);
            if (t1 < 0) t1 = L1;
            const double y = double(ap_[t3]) - y1;
            ap_[t2] = float(k65 * y + a3F);
            if (--t2 < 0) t2 = L1;
            if (--t3 < 0) t3 = L1;
            if (--w < 0) w = L1;
            // comb: the damped state is stored as float (fst) but its register value feeds the line
            auto comb = [&](std::vector<float>& buf, int& tap, float& st) {
                if (--tap < 0) tap = L1;
                const double r = double(fb) * buf[tap];
                const double ns = (r - st) * dampCoef_ + st;
                st = float(ns);
                buf[w] = float(ns + y);
                return r;
            };
            float accR = float(comb(comb_[5], c5, damp_[5]));
            float accL = float(comb(comb_[4], c4, damp_[4]));
            accR = float(accR + comb(comb_[3], c3, damp_[3]));
            accL = float(accL + comb(comb_[2], c2, damp_[2]));
            accR = float(accR + comb(comb_[1], c1, damp_[1]));
            const double r0 = comb(comb_[0], c0, damp_[0]);
            io[2 * i] = float((((double(accL) + r0) + y) - inL) * mix_ + inL);
            io[2 * i + 1] = float(((double(accR) + y) - inR) * mix_ + inR);
        }
        w_ = apw;
        preW_ = pw;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; break;
            case 2: D_ = sr_ * 0.09f; fb_ = float(double(-0.58f) - double(v) * double(0.4f)); break;
            case 3: predelay_ = int(double(v) * sr_ * v * double(0.399f)); break;
            case 4: dampCoef_ = float(1.0 - x87exp(-((double(v) * v * v * 90000.0 + 10000.0) * invSr_))); break;
            case 5: hpCoef_ = float(double(v) * invSr_ * 2000.0); break;
            case 6: lpCoef_ = float(1.0 - x87exp(-((double(v) * v * v * 90000.0 + 10000.0) * invSr_))); break;
            default: break;
        }
    }
    int L_ = 0, w_ = 0, preW_ = 0, predelay_ = 0;
    std::vector<float> pre_, ap_, comb_[6];
    float D_ = 0, fb_ = -0.78f, mix_ = 0.5f, dampCoef_ = 1, hpCoef_ = 0, lpCoef_ = 1;
    float lp_ = 0, hp_ = 0, damp_[6] = {};
};

}  // namespace

std::unique_ptr<Effect> make_fx_1() { return std::make_unique<HallReverb>(); }

}  // namespace hs2
