// 56 Rotary Speaker 2 (ctor FUN_10037560, vtable 0x100a9494: setSampleRate FUN_10037690, process FUN_10037840,
// setParam FUN_10038050; init/tempo 0x10002170 is a no-op).
// Mono sum s = 2(L+R) -> drive (x = clamp(8 p3 s, +-2), x - x^3/12, one-pole smoothed) -> a 2-pole SVF-ish
// low/high split at exp(1.386 p9 + 8.7) Hz: the low part (q2) goes to the "drum", the high part (r1) is written
// to a 50 ms line read by two Doppler taps (horn front/back, glide-to-target read velocities as in Chorus).
// Every 16 samples the horn phase (0..16, through a 17-point amplitude table, back tap at phase-8) and the drum
// phase (0..1) advance by speeds that bang-bang accelerate towards targets (slow / stop / fast from trunc(p2*4.9),
// p8 / p7 trim). Horn gains glide linearly over the 16 samples. p0 mix (dry 1-p0, wet (2-p0)p0(2.5 p11+1.5)),
// p4 lo/hi mix, p5 width, p6 horn Doppler depth, p10 horn resonance, p11 boost. On stop, the horn phase eases to
// 2.2 (or 16.1) and the drum to 0.33. Parameter p1 makes the next two blocks jump straight to the target speeds.
// The emulated x87 runs at 53-bit precision (MSVC default), so "extended" maths is done in double here.
// this+0x78 is the part's note-pitch pointer (vtable +8, FUN_1002b410). The pitch value is never read; once per block the
// original only WRITES fmod(16 - hornPhase, 8) to ptr[1] (part+0x96c, presumably a GUI horn position). Not ported:
// it has no effect on the audio and setPitchSource gives a const pointer.
// Oracle-verified: bit-exact (-240 dB) over --random 8, all 0, all 1, slow/stop/fast, with and without a pitch source.
// (An earlier -136 dB residue was the horn resonance constant: 0.6f, not 0.6.)
#include <cstdint>

#include "effect_base.h"

namespace hs2 {
namespace {

class RotarySpeaker2 : public EffectBase {
public:
    RotarySpeaker2() {
        static const uint32_t kTab[17] = {0x3e9eb852, 0x3e8f5c29, 0x3ea3d70a, 0x3e051eb8, 0x3d4ccccd, 0x3e75c28f,
                                          0x3f000000, 0x3f451eb8, 0x3f7851ec, 0x3f2b851f, 0x3ea8f5c3, 0x3e8f5c29,
                                          0x3e570a3d, 0x3e8f5c29, 0x3e23d70a, 0x3ec28f5c, 0x3e9eb852};
        std::memcpy(tab_, kTab, sizeof tab_);
    }
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * double(0.01f));  // +0xc tail: trunc(sr * 0.01f) (440 at 44.1k)
        sr_ = sr;
        const double inv = 1.0 / double(sr);
        invSr_ = float(inv);
        counter_ = 1;
        phH_ = spdH_ = phR_ = spdR_ = 0.f;
        lp_ = q1_ = q2_ = r1_ = r2_ = 0.f;
        w_ = idx1_ = idx2_ = 0;
        f1_ = f2_ = v1_ = v2_ = 0.f;
        gL_ = gLinc_ = gR_ = gRinc_ = 0.f;
        z1_ = z2_ = z3_ = 0.f;
        accH_ = float(inv * inv * 22000.0);
        accR_ = float(inv * inv * 300.0);
        base_ = float(double(sr) * double(0.0004f) + 1.0);
        f_ = float(inv * 20000.0);
        f3_ = float(inv * 1200.0);
        const int L = int(double(sr) * double(0.05f));
        if (L != L_) {
            L_ = L;
            buf_.assign(size_t(L_) + 2, 0.f);
        } else {
            std::fill(buf_.begin(), buf_.begin() + L_, 0.f);
        }
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // Gate (FUN_10037840): standard, except that a skipped (silent, expired) block still eases the horn speed towards
    // its target and advances the horn phase: fmod(phase + speed * n / 16, 16) (the drum is not advanced).
    void processGated(float* lr, int n, bool& silent) override {
        if (buf_.empty() || L_ <= 0) return;
        if (silent && gateCount_ < 0) {
            const float nf = float(n);
            double f = double(nf) * double(invSr_);
            if (f > double(0.1f)) f = double(0.1f);
            spdH_ = float((double(tgtH_) - double(spdH_)) * f + double(spdH_));
            phH_ = float(std::fmod(double(spdH_) * double(nf) * 0.0625 + double(phH_), 16.0));
            return;
        }
        Effect::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        if (buf_.empty() || L_ <= 0) return;
        const int L1 = L_ - 1;
        const float dry = float(1.0 - double(mix_));
        const double a = ((double(boost_) * 2.5 + 1.5) * (2.0 - double(mix_))) * double(mix_);
        const float A = float(a);
        const double b = a * double(lohi_) * double(0.333f);
        const float B = float(b);
        const float C = float(b * double(0.4f));
        if (hold_ > 0) {
            --hold_;
            spdH_ = tgtH_;
            spdR_ = tgtR_;
            if (tgtH_ == 0.f) {
                phH_ = 2.2f;
                phR_ = 0.33f;
            }
        }
        if (tgtH_ == 0.f) {  // stopped: ease the rotors to their rest positions
            double f = double(n) * double(invSr_);
            if (f > double(0.1f)) f = double(0.1f);
            const double target = phH_ <= 2.3f ? double(2.2f) : double(16.1f);
            phH_ = float((target - double(phH_)) * f + double(phH_));
            phR_ = float((double(0.33f) - double(phR_)) * f + double(phR_));
        }
        for (int i = 0; i < n; ++i) {
            if (--counter_ == 0) update(A, B);
            const float inL = io[2 * i], inR = io[2 * i + 1];
            double s = double(inR) + double(inL);
            s = s + s;
            float x = float(s * double(drive8_));
            uint32_t bits;
            std::memcpy(&bits, &x, 4);
            if (bits & 0x40000000u) {
                bits &= 0xc0000000u;
                std::memcpy(&x, &bits, 4);
            }
            --w_;
            const double g = s * double(boost_);
            const double xd = x;
            const double lp = ((((xd - xd * xd * xd * double(0.083333f)) - s) - g) - double(lp_)) * double(lpc_) +
                              double(lp_);
            lp_ = float(lp);
            const double sp = s + (lp - g) * double(k2_);
            const double z1 = double(z1_) - ((sp + double(z2_)) + double(z1_)) * double(f_);
            z1_ = float(z1);
            const double z2 = z1 * double(f_) + double(z2_);
            z2_ = float(z2);
            const double z3 = (z2 - double(z3_)) * double(f3_) + double(z3_);
            z3_ = float(z3);
            const double v = (z3 + z2) - (double(q1_) + double(q2_));
            const double q1 = v * double(fc_) * double(0.33f) + double(q1_);
            q1_ = float(q1);
            q2_ = float(q1 * double(fc_) * double(0.33f) + double(q2_));
            const double r1 = double(r1_) - ((double(reso_) * double(r1_) + v) + double(r2_)) * double(fc_);
            r1_ = float(r1);
            r2_ = float(r1 * double(fc_) + double(r2_));
            buf_[w_ + 1] = r1_;
            if (w_ < 0) {
                buf_[L_] = buf_[0];
                w_ = L1;
            }
            double t = buf_[idx1_];
            const double y1 = (double(buf_[idx1_ + 1]) - t) * double(f1_) + t;
            f1_ = float(double(v1_) + double(f1_));
            while (f1_ < 0.f) {
                --idx1_;
                f1_ = float(double(f1_) + 1.0);
                if (idx1_ < 0) idx1_ = L1;
            }
            t = buf_[idx2_];
            const double y2 = (double(buf_[idx2_ + 1]) - t) * double(f2_) + t;
            f2_ = float(double(v2_) + double(f2_));
            while (f2_ < 0.f) {
                --idx2_;
                f2_ = float(double(f2_) + 1.0);
                if (idx2_ < 0) idx2_ = L1;
            }
            const double OL = ((y1 + double(r1_)) * double(gL_) + double(q2_) * double(dL_)) + y2 * double(C);
            const double OR = ((y2 + double(r1_)) * double(gR_) + double(q2_) * double(dR_)) + y1 * double(C);
            const float ORf = float(OR);
            const float W = float((OR - OL) * double(width_));
            io[2 * i] = float((double(inL) * double(dry) + OL) + double(W));
            io[2 * i + 1] = float((double(inR) * double(dry) + double(ORf)) - double(W));
            gL_ = float(double(gL_) + double(gLinc_));
            gR_ = float(double(gR_) + double(gRinc_));
        }
    }

private:
    // every 16 samples: advance horn and drum, new horn gain ramps and Doppler read velocities, drum pan gains
    void update(float A, float B) {
        const float accH = spdH_ < tgtH_ ? accH_ : -accH_;
        const double sh = double(accH) + double(spdH_);
        spdH_ = float(sh);
        const double ph = sh + double(phH_);
        phH_ = float(ph);
        if (!(ph < 16.0)) phH_ = float(ph - 16.0);
        const int i = int(phH_);
        const double fr = double(phH_) - double(i);
        int d = idx1_ - w_;
        const double t1 = (double(tab_[i + 1]) - double(tab_[i])) * fr + double(tab_[i]);
        gLinc_ = float(((1.5 * t1 + double(0.2f)) * double(B) - double(gL_)) * 0.0625);
        if (d < 0) d += L_;
        double e = (((t1 * double(fm_) + double(base_)) - double(f1_)) - double(d)) * 0.0625 - 1.0;
        v1_ = e > 0.0 ? 0.f : float(e);
        const int j = (i - 8) & 15;
        const double t2 = (double(tab_[j + 1]) - double(tab_[j])) * fr + double(tab_[j]);
        d = idx2_ - w_;
        gRinc_ = float(((1.5 * t2 + double(0.2f)) * double(B) - double(gR_)) * 0.0625);
        if (d < 0) d += L_;
        e = (((t2 * double(fm_) + double(base_) * 1.25) - double(f2_)) - double(d)) * 0.0625 - 1.0;
        v2_ = e > 0.0 ? 0.f : float(e);
        const float accR = spdR_ < tgtR_ ? accR_ : -accR_;
        const double sr = double(accR) + double(spdR_);
        spdR_ = float(sr);
        const double pr = sr + double(phR_);
        phR_ = float(pr);
        if (pr > 1.0) phR_ = float(pr - 1.0);
        const float D = float(double(A) - double(A) * double(lohi_));
        const double vD = (phR_ > 0.5f ? 1.0 - double(phR_) : double(phR_)) * double(D);
        double u = (phR_ > 0.67f ? double(-0.67f) : double(0.33f)) + double(phR_);
        if (u > 0.5) u = 1.0 - u;
        dL_ = float(u * double(D) * double(0.7f) + double(0.05f));
        dR_ = float(vD * double(0.7f) + double(0.05f));
        counter_ = 16;
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: hold_ = 2; return;
            case 2: case 7: case 8:
                switch (int(double(p_[2]) * double(4.9f))) {
                    case 0: case 1: {
                        const double f = (double(p_[8]) + 0.5) * double(invSr_);
                        tgtH_ = float(f * double(217.6f));
                        tgtR_ = float(f * double(22.72f));
                        return;
                    }
                    case 2: tgtH_ = tgtR_ = 0.f; return;
                    case 3: case 4: {
                        const double f = (double(p_[7]) + 0.5) * double(invSr_);
                        tgtH_ = float(f * double(1669.12f));
                        tgtR_ = float(f * double(92.32f));
                        return;
                    }
                    default: return;
                }
            case 3:
                drive8_ = float(double(v) * 8.0);
                lpc_ = float((double(v) * double(v) * 5000.0 + 3160.0) * double(invSr_));
                k2_ = float((2.0 - double(v)) * double(v));
                return;
            case 4: lohi_ = float(double(v) * double(0.9f) + double(0.05f)); return;
            case 5: width_ = float(0.5 - double(v) * double(0.65f)); return;
            case 6: fm_ = float(double(v) * double(sr_) * double(0.00136f)); return;
            case 9:
                fc_ = float(std::exp2((double(v) * double(1.386f) + double(8.7f)) * 1.4426950408889634) *
                            double(invSr_));
                return;
            case 10: reso_ = float(double(0.6f) - double(v) * double(0.35f)); return;
            case 11: boost_ = v; return;
            default: return;
        }
    }
    float tab_[17];
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, idx1_ = 0, idx2_ = 0, counter_ = 1, hold_ = 2;
    float mix_ = 0.5f, boost_ = 0, drive8_ = 0, lpc_ = 0, k2_ = 0, lohi_ = 0, width_ = 0, fm_ = 0, fc_ = 0, reso_ = 0;
    float base_ = 1, f_ = 0, f3_ = 0, accH_ = 0, accR_ = 0;
    float tgtH_ = 0, tgtR_ = 0, spdH_ = 0, spdR_ = 0, phH_ = 0, phR_ = 0;
    float lp_ = 0, z1_ = 0, z2_ = 0, z3_ = 0, q1_ = 0, q2_ = 0, r1_ = 0, r2_ = 0;
    float f1_ = 0, f2_ = 0, v1_ = 0, v2_ = 0, gL_ = 0, gLinc_ = 0, gR_ = 0, gRinc_ = 0, dL_ = 0, dR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_56() { return std::make_unique<RotarySpeaker2>(); }

}  // namespace hs2
