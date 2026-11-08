// 41 Deep Phaser (ctor FUN_10030140, setSampleRate FUN_10030180, setParam FUN_100305f0, process FUN_10030260;
// vtable 0x100a9088, init/tempo 0x10002170 is a no-op).
// Mono (L + R) input, minus feedback (0.8 p4) of the previous L output, through 12 first-order all-pass sections
// sharing coefficient g1: L = dry L + wet * section 12. R = dry R + wet * (section 11 output through one more
// all-pass with coefficient g2). Every 32 samples a parabolic LFO (phase in (-1, 1]; R phase offset by 1) sets a
// per-sample glide to the target ph^2 * depth + centre. The L target is bent by an envelope follower: the sum of
// |L + R| over the 32 samples is smoothed (coefficient 1 - exp(-((1 - p7)^2 * 5000 + 10) / sr)) to E; with
// s = p6 - 0.5, target = (T - 20000/sr) * exp(-E s) + 20000/sr for s >= 0, else (T + 1) * exp(E s) - 1.
// p0 mix (dry 1-m^2, wet m-m^2/2), p1/p2 rate (last set wins), p3 depth ((160 p3)^2 / sr), p4 feedback,
// p5 centre (((29.5 p5)^3 + 500) / sr - 1), p6 envelope amount, p7 envelope speed. Ctor defaults p4 = 0,
// p5 = p6 = 0.5.
// The original keeps g1/g2 on the x87 stack for a whole process() call (53-bit precision control) and stores them
// as float at the end; the port does the same per call. Every float store / unrounded intermediate of the
// all-pass chain is mirrored exactly.
// Oracle-verified: bit-exact (-240 dB) on --random 8 plus all-0, all-1 and hand-picked sets.
#include "effect_base.h"

namespace hs2 {
namespace {

// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale (53-bit precision control).
double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}

class DeepPhaser : public EffectBase {
public:
    DeepPhaser() {
        p_[4] = 0.f;
        p_[5] = p_[6] = 0.5f;
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        top_ = float((1.0 / double(sr)) * 20000.0);
        count_ = 1;
        g1_ = g2_ = v1_ = v2_ = ph_ = 0;
        std::fill(std::begin(z_), std::end(z_), 0.f);
        zR_ = s_ = acc_ = env_ = 0;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        const double m2 = double(mix_) * double(mix_);
        const float dry = float(1.0 - m2), wet = float(double(mix_) - m2 * 0.5);
        double g1 = g1_, g2 = g2_;
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                count_ = 32;
                const double s = double(inc_) + double(ph_);
                ph_ = float(s);
                if (1.0 < s) ph_ = float(s - 2.0);
                double t = double(ph_) * double(ph_) * double(depth_) + double(centre_);
                const double e = (double(acc_) - double(env_)) * double(envCoef_) + double(env_);
                env_ = float(e);
                const double es = e * double(sens_);
                if (sens_ >= 0.f)
                    t = (t - double(top_)) * x87exp(-es) + double(top_);
                else
                    t = (t + 1.0) * x87exp(es) - 1.0;
                acc_ = 0;
                v1_ = float((t - g1) * 0.03125);
                double s2 = double(stereo_) + double(ph_);
                if (1.0 < s2) s2 -= 2.0;
                v2_ = float(((s2 * double(depth_) * s2 + double(centre_)) - g2) * 0.03125);
            }
            g2 += double(v2_);
            g1 += double(v1_);
            const float inL = io[2 * i], inR = io[2 * i + 1];
            const double x0 = double(inR) + double(inL);
            acc_ = float(std::fabs(x0) + double(acc_));
            const double g = g1;
            float* z = z_;
            // sections 1..12 (P = product passed on, F = float-stored output)
            const double x = x0 - double(fb_) * double(s_);
            const double a = x * g + z[0];
            const double p0 = a * g;
            z[0] = float(x - p0);
            const double b = p0 + z[1];
            const float bf = float(b);
            const float p1 = float(b * g);
            z[1] = float(a - p1);
            const double c = double(p1) + z[2];
            const double p2 = c * g;
            z[2] = float(double(bf) - p2);
            const double d = p2 + z[3];
            const double p3 = d * g;
            z[3] = float(c - p3);
            const double e = p3 + z[4];
            const float ef = float(e);
            const float p4 = float(e * g);
            z[4] = float(d - p4);
            const double f = double(p4) + z[5];
            const double p5 = f * g;
            z[5] = float(double(ef) - p5);
            const double h = p5 + z[6];
            const double p6 = h * g;
            z[6] = float(f - p6);
            const double k = p6 + z[7];
            const float kf = float(k);
            const float p7 = float(k * g);
            z[7] = float(h - p7);
            const double l = double(p7) + z[8];
            const double p8 = l * g;
            z[8] = float(double(kf) - p8);
            const double mm = p8 + z[9];
            const double p9 = mm * g;
            z[9] = float(l - p9);
            const double q = p9 + z[10];
            const float qf = float(q);
            const float p10 = float(q * g);
            z[10] = float(mm - p10);
            const double y = double(p10) + z[11];
            z[11] = float(double(qf) - y * g);
            s_ = float(y);
            io[2 * i] = float(double(inL) * dry + y * wet);
            const double r = double(qf) * g2 + zR_;
            zR_ = float(double(qf) - r * g2);
            io[2 * i + 1] = float(double(inR) * dry + r * wet);
        }
        g1_ = float(g1);
        g2_ = float(g2);
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2: {
                const double r = x87exp(double(v) * double(5.991464f) - double(2.995732f));
                inc_ = float(r * double(invSr_) * 64.0);
                return;
            }
            case 3: {
                const double a = double(v) * 160.0;
                depth_ = float(a * double(invSr_) * a);
                return;
            }
            case 4: fb_ = float(double(v) * double(0.8f)); return;
            case 5: {
                const double c = double(v) * 29.5;
                centre_ = float((c * c * c + 500.0) * double(invSr_) - 1.0);
                return;
            }
            case 6: sens_ = float(double(v) - 0.5); return;
            case 7: {
                const double u = 1.0 - double(v);
                envCoef_ = float(1.0 - x87exp(-((u * u * 5000.0 + 10.0) * double(invSr_))));
                return;
            }
            default: return;
        }
    }
    int count_ = 1;
    float mix_ = 0.5f, inc_ = 0, depth_ = 0, fb_ = 0, centre_ = 0, sens_ = 0, envCoef_ = 0, top_ = 0;
    const float stereo_ = 1.f;  // +0x90, set to 1.0 by setSampleRate, no parameter
    float g1_ = 0, g2_ = 0, v1_ = 0, v2_ = 0, ph_ = 0;
    float z_[12] = {}, zR_ = 0, s_ = 0, acc_ = 0, env_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_41() { return std::make_unique<DeepPhaser>(); }

}  // namespace hs2
