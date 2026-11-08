// 55 Rotary Speaker (ctor FUN_100367d0, vtable 0x100a93f0: setSampleRate FUN_10036800, process FUN_100369b0,
// setParam FUN_10036ff0; init/tempo 0x10002170 is a no-op).
// Mono sum m = L+R: a state-variable low/high split at (200 + 2400 p4^2) Hz gives the "drum" (low, cc) and the
// horn (high). The horn signal is soft-clipped "dirt" (clamp(10 p3 z, +-p3)) subtracted from a 1 ms line read
// at a cos-modulated position (13.2..39.7 samples at 44.1k), then a tone SVF (p10). Each channel runs the sum
// through a 5-stage first-order allpass chain whose coefficient (A for L, B for R) glides every 32 samples to
// 42000/sr * ph^2 + 6600/sr - 1 of the horn phase ph (L) / ph + p5 (R): Doppler by allpass dispersion.
// Output: L = (gainL * cc - chainL) * G + L * dry, with drum AM gainL = min(|drum phase| + 0.7, 2) (R uses the
// phase + p5), G = (0.7 - 0.3 p3) * p0/2, dry = 1 - 2 (p0/2)^2. Speeds bang-bang accelerate (p8 horn, p9 drum)
// towards slow / stop / fast targets from trunc(2.9 p2) (p6 horn, p7 drum speed trims); p1 makes the next two
// blocks jump to the targets. Quirk: the read position wraps by L-1, not L. The allpass coefficients and the
// running ramp stay x87-extended inside a block (rounded to float at block end) - mirrored with doubles; the
// emulated x87 runs at 53-bit precision, so "extended" maths is double here.
// this+0x78 is the part's note-pitch pointer (vtable +8, FUN_1002b410). The pitch value is never read; once per block the
// original only WRITES fmod(8 - 8*hornPhase, 8) to ptr[1] (part+0x96c, presumably a GUI horn position). Not ported:
// it has no effect on the audio and setPitchSource gives a const pointer.
// Oracle-verified: bit-exact (-240 dB) over --random 8 and all 0 / all 1 / slow / stop / fast.
#include "effect_base.h"

namespace hs2 {
namespace {

class RotarySpeaker : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * double(0.01f));  // +0xc tail: trunc(sr * 0.01f) (440 at 44.1k)
        sr_ = sr;
        const double inv = 1.0 / double(sr);
        invSr_ = float(inv);
        count_ = 1;
        A_ = B_ = dA_ = dB_ = 0.f;
        phH_ = spdH_ = phD_ = spdD_ = 0.f;
        cc_ = d0_ = d8_ = dc_ = 0.f;
        std::memset(apL_, 0, sizeof apL_);
        std::memset(apR_, 0, sizeof apR_);
        a4_ = float(6600.0 * inv - 1.0);
        a8_ = float(inv * 42000.0);
        const int L = int(double(sr) * double(0.001f));
        if (L != L_) {
            L_ = L;
            buf_.assign(size_t(std::max(L_, 0)) + 2, 0.f);
            w_ = r_ = 0;
            frac_ = 0.f;
        } else {
            std::fill(buf_.begin(), buf_.begin() + L_, 0.f);
        }
        modDepth_ = float(double(sr) * double(0.0003f));
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // Gate (FUN_100369b0): standard, except that a skipped (silent, expired) block still eases the horn speed towards
    // its target and advances the horn phase by speed * n / 32 (the drum is not advanced).
    void processGated(float* lr, int n, bool& silent) override {
        if (buf_.empty() || L_ <= 0) return;
        if (silent && gateCount_ < 0) {
            double f = double(n) * double(invSr_);
            if (f > double(0.1f)) f = double(0.1f);
            const double sp = (double(tgtH_) - double(spdH_)) * f + double(spdH_);
            spdH_ = float(sp);
            const double ph = sp * double(n) * 0.03125 + double(phH_);
            phH_ = float(ph);
            if (ph > 1.0) {
                double q = phH_;
                do q -= 2.0; while (q > 1.0);
                phH_ = float(q);
            }
            return;
        }
        Effect::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        if (buf_.empty() || L_ <= 0) return;
        const int L1 = L_ - 1;
        double A = A_, B = B_;
        float dA = dA_, dB = dB_;
        const float G = float((double(0.7f) - double(dirt_) * double(0.3f)) * double(b8_));
        const float D = float(1.0 - (double(b8_) * double(b8_) + double(b8_) * double(b8_)));
        if (hold_ > 0) {
            --hold_;
            spdD_ = tgtD_;
            spdH_ = tgtH_;
            if (tgtH_ == 0.f) phD_ = phH_ = float(double(width_) * 0.5);
        }
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                count_ = 32;
                // horn
                const float accH = spdH_ < tgtH_ ? accH_ : -accH_;
                double s = double(accH) + double(spdH_);
                spdH_ = float(s);
                s += double(phH_);
                phH_ = float(s);
                if (s > 1.0) phH_ = float(s - 2.0);
                dA = float((((double(a8_) * double(phH_)) * double(phH_) + double(a4_)) - A) * 0.03125);
                double q = double(width_) + double(phH_);
                if (q > 1.0) q -= 2.0;
                dB = float((((q * double(a8_)) * q + double(a4_)) - B) * 0.03125);
                const double pos =
                    (std::cos((double(width_) * 0.5 + double(phH_)) * double(3.1415927f)) + 2.0) * double(modDepth_);
                const int ip = int(pos);
                frac_ = float(pos - double(ip));
                r_ = ip + w_;
                if (L1 < r_) r_ -= L1;  // (sic)
                // drum
                const float accD = spdD_ < tgtD_ ? accD_ : -accD_;
                s = double(accD) + double(spdD_);
                spdD_ = float(s);
                s += double(phD_);
                phD_ = float(s);
                if (s > 2.0) phD_ = float(s - 4.0);
                double g = std::fabs(double(phD_)) + double(0.7f);
                gL_ = g > 2.0 ? 2.f : float(g);
                double t = double(width_) + double(phD_);
                if (t > 2.0) t -= 4.0;
                g = std::fabs(t) + double(0.7f);
                gR_ = g > 2.0 ? 2.f : float(g);
            }
            const float inL = io[2 * i], inR = io[2 * i + 1];
            const double sum = ((double(inR) + double(d0_)) + double(inL)) + double(cc_);
            const double z = double(d0_) - sum * double(fc1_);
            d0_ = float(z);
            cc_ = float(z * double(fc1_) + double(cc_));
            double h = z * double(dirt_) * 10.0;
            if (h > double(dirt_)) {
                h = dirt_;
            } else {
                const float nd = -dirt_;
                if (h < double(nd)) h = nd;
            }
            buf_[w_] = float(double(inR) + double(inL));
            if (--w_ < 0) {
                w_ = L1;
                buf_[L_] = buf_[0];
            }
            const double y = (double(buf_[r_ + 1]) - double(buf_[r_])) * double(frac_) + double(buf_[r_]);
            if (--r_ < 0) r_ = L1;
            const double v = y - h * double(dirt_);
            const double u = double(d8_) - ((double(dc_) + double(d8_)) + v) * double(fc2_);
            d8_ = float(u);
            dc_ = float(u * double(fc2_) + double(dc_));
            const double m = sum + double(d8_);
            // L chain (coefficient A)
            float outL;
            {
                float* st = apL_;
                const double p1 = m * A + double(st[0]);
                const double p1A = p1 * A;
                st[0] = float(m - p1A);
                const double p2 = p1A + double(st[1]);
                const double p2A = p2 * A;
                st[1] = float(p1 - p2A);
                const double p3 = p2A + double(st[2]);
                const float p3f = float(p3);
                const float q3 = float(p3 * A);
                st[2] = float(p2 - double(q3));
                const double p4 = double(q3) + double(st[3]);
                const double p4A = p4 * A;
                st[3] = float(double(p3f) - p4A);
                const double p5 = p4A + double(st[4]);
                const float p5f = float(p5);
                st[4] = float(p4 - p5 * A);
                A += double(dA);
                outL = float((double(gL_) * double(cc_) - double(p5f)) * double(G) + double(inL) * double(D));
            }
            // R chain (coefficient B)
            float outR;
            {
                float* st = apR_;
                const double p1 = m * B + double(st[0]);
                const double p1B = p1 * B;
                st[0] = float(m - p1B);
                const double p2 = p1B + double(st[1]);
                const float p2f = float(p2);
                const float q2 = float(p2 * B);
                st[1] = float(p1 - double(q2));
                const double p3 = double(q2) + double(st[2]);
                const double p3B = p3 * B;
                st[2] = float(double(p2f) - p3B);
                const double p4 = p3B + double(st[3]);
                const double p4B = p4 * B;
                st[3] = float(p3 - p4B);
                const double p5 = p4B + double(st[4]);
                const float p5f = float(p5);
                st[4] = float(p4 - p5 * B);
                B += double(dB);
                outR = float((double(gR_) * double(cc_) - double(p5f)) * double(G) + double(inR) * double(D));
            }
            io[2 * i] = outL;
            io[2 * i + 1] = outR;
        }
        A_ = float(A);
        B_ = float(B);
        dA_ = dA;
        dB_ = dB;
    }

private:
    static double e87(double x) { return std::exp2(x * 1.4426950408889634); }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: b8_ = float(double(v) * 0.5); return;
            case 1: hold_ = 2; return;
            case 2: case 6: case 7: {
                const double x6 = double(p_[6]) - 0.5, x7 = double(p_[7]) - 0.5;
                const float fH = float(e87(x6 + x6) * double(invSr_) * 64.0);
                const float fD = float(e87(x7 + x7) * double(invSr_) * 64.0);
                switch (int(double(p_[2]) * double(2.9f))) {
                    case 0: tgtH_ = tgtD_ = 0.f; return;
                    case 1:
                        tgtH_ = float(double(fH) * double(0.85f));
                        tgtD_ = float(double(fD) * double(1.42f));
                        return;
                    case 2:
                        tgtH_ = float(double(fH) * 7.5);
                        tgtD_ = float(double(fD) * double(6.63f));
                        return;
                    default: return;
                }
            }
            case 3: dirt_ = v; return;
            case 4: fc1_ = float((double(v) * double(v) * 2400.0 + 200.0) * double(invSr_) * double(6.2831855f)); return;
            case 5: width_ = v; return;
            case 8:
                accH_ = float(e87((double(v) - 0.5) * double(4.6f)) * double(invSr_) * double(invSr_) * 14000.0);
                return;
            case 9:
                accD_ = float(e87((double(v) - 0.5) * double(4.6f)) * double(invSr_) * double(invSr_) * 7000.0);
                return;
            case 10: fc2_ = float((double(v) * double(v) * 2400.0 + 200.0) * double(invSr_) * double(6.2831855f)); return;
            default: return;
        }
    }
    std::vector<float> buf_;
    int L_ = 0, w_ = 0, r_ = 0, count_ = 1, hold_ = 2;
    float frac_ = 0, modDepth_ = 0, a4_ = 0, a8_ = 0;
    float b8_ = 0.25f, dirt_ = 0, width_ = 0, fc1_ = 0, fc2_ = 0, accH_ = 0, accD_ = 0, tgtH_ = 0, tgtD_ = 0;
    float A_ = 0, B_ = 0, dA_ = 0, dB_ = 0, phH_ = 0, spdH_ = 0, phD_ = 0, spdD_ = 0, gL_ = 0, gR_ = 0;
    float cc_ = 0, d0_ = 0, d8_ = 0, dc_ = 0, apL_[5] = {}, apR_[5] = {};
};

}  // namespace

std::unique_ptr<Effect> make_fx_55() { return std::make_unique<RotarySpeaker>(); }

}  // namespace hs2
