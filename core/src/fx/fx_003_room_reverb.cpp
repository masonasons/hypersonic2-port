// 3 Room Reverb (ctor FUN_10029580, setSampleRate FUN_100295e0, process FUN_10029810, setParam FUN_1002a5d0;
// vtable 0x100a8c44, init/tempo 0x10002170 unused).
// Stereo predelay (0.33 s) -> per-channel lowpass (p6) + low cut (p5) -> four 6-stage allpass chains (gains
// 0.7..0.2, delay lengths prime multiples of size*sr/48000) cross-coupled through a feedback matrix (p2 * 0.7);
// chains A/B have damped (p4) feedback states. Output mixes the 3rd-stage taps and the chain ends by p8 (blend),
// p0 mix. p3 predelay, p7 size (applied after a 0.2 s hold once running). The x87 code keeps several stage values
// in 53-bit registers while others are stored to float: mirrored here with double intermediates.
// Oracle-verified: bit-exact (-240 dB floor) over --random 8 plus all-0 / all-1 / defaults.
#include "effect_base.h"

namespace hs2 {
namespace {

class RoomReverb : public EffectBase {
public:
    RoomReverb() {
        p_[5] = 0.5f;  // in_ECX[0x17]
        p_[6] = 0.75f;  // in_ECX[0x18]
    }
    void setSampleRate(float sr) override {
        gate_ = Gate::Detect;  // keeps running while its output is audible (FUN_10025480)
        sr_ = sr;
        invSr_ = 1.f / sr;
        L_ = int(double(sr) * double(0.33f));
        pre_.assign(size_t(L_) * 2, 0.f);
        for (auto& b : ap_) b.assign(L_, 0.f);
        w_ = 0;
        lp_[0] = lp_[1] = hp_[0] = hp_[1] = 0;
        s9c_ = sa0_ = sa4_ = sa8_ = 0;
        reapplyParams();
        hold_ = 0;
        running_ = false;
    }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        if (hold_ < 1 || !running_)
            size_ = float(double(p_[7]) * double(0.9f) + double(0.1f));
        else
            hold_ -= n;
        const int L = L_, L1 = L - 1;
        const double t = double(size_) * double(sr_) * double(2.0833333e-05f);
        const double k = kfb_, damp = damp_;
        const double p8 = p_[8];
        const double w8 = float((p8 + p8) - p8 * p8);
        const double d8 = float(1.0 - p8 * p8);
        const double mix = mix_;
        running_ = true;
        int w = w_;
        int pw = 2 * w;
        int pr = pw + 2 * predelay_;
        if (2 * L1 < pr) pr -= 2 * L;
        static const int kLen[4][6] = {{487, 719, 1069, 1567, 2309, 3407},
                                       {509, 761, 1123, 1657, 2447, 3607},
                                       {659, 971, 1433, 2113, 3119, 4603},
                                       {739, 1093, 1613, 2381, 3511, 5179}};
        int tap[4][6];
        for (int c = 0; c < 4; ++c) {
            int x = w;
            for (int s = 0; s < 6; ++s) {
                x -= int(double(-kLen[c][s]) * t);
                if (L1 < x) x -= L;
                tap[c][s] = x;
            }
        }
        const double lpc = lpCoef_, hpc = hpCoef_;
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            pw -= 2;
            pre_[pw + 2] = inL;
            pre_[pw + 3] = inR;
            if (pw < 0) pw = 2 * L1;
            const double eps = double(9.99999993922529e-09f);
            double lp = ((double(pre_[pr]) + eps) - lp_[0]) * lpc + lp_[0];
            lp_[0] = float(lp);
            double x = lp - hp_[0];
            const float xL = float(x);
            hp_[0] = float(x * hpc + hp_[0]);
            lp = ((double(pre_[pr + 1]) + eps) - lp_[1]) * lpc + lp_[1];
            lp_[1] = float(lp);
            x = lp - hp_[1];
            const float xR = float(x);
            hp_[1] = float(x * hpc + hp_[1]);
            pr -= 2;
            if (pr < 0) pr = 2 * L1;
            const float sumD = float(double(sa0_) + double(s9c_));
            double v3, v6;
            float v3f;
            // chain A
            chain(ap_[0], tap[0], w, (double(s9c_) - sa0_) * k + xL, v3, v3f, v6);
            const double v3A = v3;
            s9c_ = float((v6 - s9c_) * damp + s9c_);
            // chain B
            chain(ap_[1], tap[1], w, (double(sa4_) - sa8_) * k + xR, v3, v3f, v6);
            const float v3B = v3f;
            sa0_ = float((v6 - sa0_) * damp + sa0_);
            // chain C
            chain(ap_[2], tap[2], w, double(xL) - (double(sa8_) + sa4_) * k, v3, v3f, v6);
            const double sumL = v3A + v3f;
            sa4_ = float(v6);
            // chain D
            chain(ap_[3], tap[3], w, double(sumD) * k + xR, v3, v3f, v6);
            const double sumR = double(v3B) + v3f;
            sa8_ = float(v6);
            if (--w < 0) w = L1;
            const double oL = sumL * d8 + (double(sa4_) + s9c_) * w8;
            io[2 * i] = float((oL - inL) * mix + inL);
            const double oR = sumR * d8 + (double(sa0_) + sa8_) * w8;
            io[2 * i + 1] = float((oR - inR) * mix + inR);
        }
        w_ = w;
    }

private:
    // One 6-stage allpass chain; stage 1 writes at the shared write index w, stage s+1 at stage s's read tap.
    void chain(std::vector<float>& B, int* t, int w, double in, double& v3, float& v3f, double& v6) {
        const int L1 = L_ - 1;
        const double g7 = double(0.7f), g6 = double(0.6f), g4 = double(0.4f), g3 = double(0.3f),
                     g2 = double(0.2f);
        const double v1 = B[t[0]] - g7 * in;
        B[w] = float(g7 * v1 + in);
        const double v2 = B[t[1]] - g6 * v1;
        const float v2f = float(v2);
        B[t[0]] = float(g6 * v2 + v1);
        if (--t[0] < 0) t[0] = L1;
        v3 = B[t[2]] - 0.5 * v2f;
        v3f = float(v3);
        B[t[1]] = float(0.5 * v3 + v2f);
        if (--t[1] < 0) t[1] = L1;
        const double v4 = B[t[3]] - g4 * v3f;
        B[t[2]] = float(g4 * v4 + v3f);
        if (--t[2] < 0) t[2] = L1;
        const double v5 = B[t[4]] - g3 * v4;
        const float v5f = float(v5);
        B[t[3]] = float(g3 * v5 + v4);
        if (--t[3] < 0) t[3] = L1;
        v6 = B[t[5]] - g2 * v5f;
        B[t[4]] = float(g2 * v6 + v5f);
        if (--t[4] < 0) t[4] = L1;
        if (--t[5] < 0) t[5] = L1;
    }
    static float expCoef(float v, float invSr) {
        const double dv = v;
        return float(1.0 - std::exp(-((dv * dv * dv * 90000.0 + 10000.0) * double(invSr))));
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; break;
            case 2: kfb_ = v * 0.7f; break;
            case 3: predelay_ = int(double(v) * double(sr_) * double(v) * 0.25); break;
            case 4: damp_ = expCoef(v, invSr_); break;
            case 5: hpCoef_ = float(double(v) * double(invSr_) * 2000.0); break;
            case 6: lpCoef_ = expCoef(v, invSr_); break;
            case 7: hold_ = int(double(sr_) * double(0.2f)); break;
            default: break;
        }
    }
    int L_ = 0, w_ = 0, predelay_ = 0, hold_ = 0;
    bool running_ = false;
    std::vector<float> pre_, ap_[4];
    float mix_ = 0.5f, kfb_ = 0, damp_ = 0, hpCoef_ = 0, lpCoef_ = 0, size_ = 0.5f;
    float lp_[2] = {}, hp_[2] = {}, s9c_ = 0, sa0_ = 0, sa4_ = 0, sa8_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_3() { return std::make_unique<RoomReverb>(); }

}  // namespace hs2
