// 110 Modulate L/R (ctor FUN_100428e0, setSampleRate FUN_10042910, setParam FUN_100433e0, process FUN_10042990).
// The left input modulates the right one; the (mono) product is added to both outputs on top of the dry signal
// scaled by 1-mix^2. Dry routing p5 "Thru" trunc(p*2.9): 0 both outputs get L, 1 L/R, 2 both get R.
// Modes p2 trunc(p*3.9):
//  0 envelope: env += (L + 1e-8 - env)*k1 (k1 = 1-exp(-((1-p3)^3*200000+200)/sr)); mod = env*R*mix*drive
//  1 peak follower of |L| (attack k2 = 1-exp(-((1-p3)^2*600+2)/sr), release k2/10); mod = env*R*mix*drive
//  2 same follower on max(0, 2-|L*drive|)^2; mod = env*R*mix
//  3 "vocoder": an octave-decimated bank of up to 8 bands (band k updated when the low k bits of a 7-bit down
//    counter are all ones). Each band: two cascaded SVF stages (f = 32000/sr*0.805 and 32000/sr, damping 0.2) on
//    L and on R, the |bandpass(L)| envelope (k3 = 1-exp(-exp((1-p3)*8+0.6)/sr)) times bandpass(R) minus half of
//    the next band's output feeds a third SVF; each band's lowpass outputs feed the next band.
//    mod = (R - Rprev)*drive/2*env0 + mix*drive*0.03*out0 (band 0 only).
// p0 mix, p3 smooth, p4 drive exp(4.6052p). All arithmetic follows the original x87 register/float-spill pattern.
// Oracle-verified: bit-exact (-240 dB) on all 4 modes and 3 thru settings, --random 8 + extremes.
#include "effect_base.h"

namespace hs2 {
namespace {

class ModulateLR : public EffectBase {
public:
    ModulateLR() { p_[4] = p_[5] = p_[6] = p_[7] = 0.5f; }
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.5);  // +0xc tail: trunc(sr * 0.5)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        cnt_ = 0;
        s80_ = 0;
        std::memset(B_, 0, sizeof B_);
        reapplyParams();
    }
    void process(float* io, int n) override {
        if (lastMode_ != mode_) {
            lastMode_ = mode_;
            s80_ = 0;
            if (mode_ == 3) std::memset(B_, 0, sizeof B_);
        }
        const float m = mix_;
        const float dry = float(1.0 - double(m) * m);
        const float rel = float(double(k88_) * double(0.1f));
        const bool d1R = thru_ == 2, d2R = thru_ != 0;
        switch (mode_) {
            case 0: case 1: case 2: {
                const float k5 = mode_ == 2 ? m : float(double(m) * drive_);
                for (int i = 0; i < n; ++i) {
                    const float L = io[2 * i], R = io[2 * i + 1];
                    double e;
                    if (mode_ == 0) {
                        e = ((double(L) + double(1e-08f)) - s80_) * k84_ + s80_;
                    } else {
                        double a;
                        if (mode_ == 1) {
                            a = std::fabs(L);
                        } else {
                            a = 2.0 - std::fabs(double(L) * drive_);
                            a = a < 0.0 ? 0.0 : a * a;  // NaN-free inputs assumed
                        }
                        const double c = a > double(s80_) ? double(k88_) : double(rel);
                        e = (a - s80_) * c + s80_;
                    }
                    s80_ = float(e);
                    const double mod = e * R * k5;
                    io[2 * i] = float(double(dry) * (d1R ? R : L) + mod);
                    io[2 * i + 1] = float(double(dry) * (d2R ? R : L) + mod);
                }
                return;
            }
            case 3: break;
            default: return;
        }
        const double c0d = double(invSr_) * 32000.0;
        const float c0 = float(c0d);
        const float c1 = float(c0d * double(0.805f));
        const float hd = float(double(drive_) * 0.5);
        const float ko = float(double(m) * drive_ * double(0.03f));
        const double p2 = double(0.2f);
        for (int i = 0; i < n; ++i) {
            const float L = io[2 * i], R = io[2 * i + 1];
            if (--cnt_ < 0) cnt_ = 0x7f;
            B_[3] = L;
            B_[7] = R;
            unsigned bits = unsigned(cnt_) * 2 + 1;
            float* F = B_;
            do {
                const double in1 = F[3], in2 = F[7];
                double t = double(F[12]) - ((p2 * F[12] + F[13]) + in1) * c1;
                F[12] = float(t);
                const double n13 = t * c1 + F[13];
                F[13] = float(n13);
                t = double(F[14]) - (n13 + (p2 * F[14] + F[15])) * c0;
                F[14] = float(t);
                F[15] = float(t * c0 + F[15]);
                t = double(F[16]) - ((p2 * F[16] + F[17]) + in2) * c1;
                F[16] = float(t);
                const double n17 = t * c1 + F[17];
                F[17] = float(n17);
                t = double(F[18]) - (((n17 + p2 * F[18]) + F[19]) * c0);
                F[18] = float(t);
                F[19] = float(t * c0 + F[19]);
                const double n23 = (std::fabs(double(F[14])) - F[23]) * k27c_ + F[23];
                F[23] = float(n23);
                const double v = n23 * F[18] - double(F[33]) * 0.5;
                t = double(F[20]) - (((double(F[20]) + F[22]) + v) + F[21]) * c1;
                F[20] = float(t);
                F[21] = float(t * c1 + F[21]);
                F[22] = float(v);
                F += 12;
                bits = unsigned(int(bits) >> 1);
            } while (bits & 1);
            const double mod = (double(R) - s80_) * hd * B_[23] + double(ko) * B_[21];
            s80_ = R;
            io[2 * i] = float(double(dry) * (d1R ? R : L) + mod);
            io[2 * i + 1] = float(double(dry) * (d2R ? R : L) + mod);
        }
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: mode_ = int(double(v) * double(3.9f)); return;
            case 3: {
                const double f = 1.0 - double(v);
                const double f2 = f * f, f3 = f2 * f;
                k84_ = float(1.0 - std::exp(-((f3 * 200000.0 + 200.0) * invSr_)));
                k88_ = float(1.0 - std::exp(-((f2 * 600.0 + 2.0) * invSr_)));
                k27c_ = float(1.0 - std::exp(-(std::exp(f * 8.0 + double(0.6f)) * invSr_)));
                return;
            }
            case 4: drive_ = float(std::exp(double(v) * double(4.6052f))); return;
            case 5: thru_ = int(double(v) * double(2.9f)); return;
            default: return;
        }
    }
    int mode_ = 0, lastMode_ = 0, thru_ = 0, cnt_ = 0;
    float mix_ = 0.5f, drive_ = 1, k84_ = 0, k88_ = 0, k27c_ = 0, s80_ = 0;
    float B_[120 + 12];  // this+0x98 .. 0x278 (120 floats); padding for safety
};

}  // namespace

std::unique_ptr<Effect> make_fx_110() { return std::make_unique<ModulateLR>(); }

}  // namespace hs2
