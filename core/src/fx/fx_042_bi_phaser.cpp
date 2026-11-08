// 42 Bi-Phaser (ctor FUN_10043830, setSampleRate FUN_1002fd50, setParam FUN_1002fbd0 (shared with the Phaser),
// process FUN_1002fdb0; vtable 0x100a9d3c, init/tempo 0x10002170 is a no-op).
// The Phaser (type 40) followed by a second 4-section all-pass chain without feedback: per channel the Phaser's
// dry/wet mix y (kept unrounded) goes through the second chain (same coefficient g) and is mixed again
// (y * dry + chain2 * wet). Parameters as the Phaser: p0 mix, p1/p2 rate, p3 depth, p4 feedback, p5 stereo,
// p6 centre; ctor defaults p4 = 0, p5 = p6 = 0.5. (setSampleRate also stores 1000 / sr at +0xfc, never used.)
// The original keeps g on the x87 stack for a whole process() call (53-bit precision control) and stores it as
// float at the end, so the port does the same per call.
// No silence gating (process ignores the 5th-arg flag, which stays set).
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

// One channel: 4 all-pass sections, z = section states, s = last chain output (feedback).
struct Chain {
    float z[4] = {}, s = 0;
    // returns the chain output as float (also stored in s)
    float run(double in, double g, float fbk) {
        const double x = double(fbk) * double(s) + in;
        const double a = x * g + double(z[0]);
        const double t = a * g;
        z[0] = float(x - t);
        const double b = t + double(z[1]);
        const float bf = float(b);
        const float u = float(b * g);
        z[1] = float(a - double(u));
        const double c = double(u) + double(z[2]);
        const double cg = c * g;
        z[2] = float(double(bf) - cg);
        const double d = cg + double(z[3]);
        s = float(d);
        z[3] = float(c - d * g);
        return s;
    }
};

// Second chain (no feedback): input y unrounded, returns the unrounded output.
struct Chain2 {
    float w[4] = {};
    double run(double y, double g) {
        const double a = y * g + double(w[0]);
        const double t = a * g;
        w[0] = float(y - t);
        const double b = t + double(w[1]);
        const double bg = b * g;
        w[1] = float(a - bg);
        const double c = bg + double(w[2]);
        const float cf = float(c);
        const float u = float(c * g);
        w[2] = float(b - double(u));
        const double e = double(u) + double(w[3]);
        w[3] = float(double(cf) - e * g);
        return e;
    }
};

class BiPhaser : public EffectBase {
public:
    BiPhaser() {
        p_[4] = 0.f;
        p_[5] = p_[6] = 0.5f;
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        count_ = 1;
        g1_ = g2_ = v1_ = v2_ = ph_ = 0;
        l_ = r_ = Chain{};
        l2_ = r2_ = Chain2{};
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        const double m2 = double(mix_) * double(mix_);
        const float dry = float(1.0 - m2), wet = float((double(mix_) + double(mix_)) - m2);
        double g1 = g1_, g2 = g2_;
        for (int i = 0; i < n; ++i) {
            if (--count_ == 0) {
                count_ = 32;
                const double s = double(inc_) + double(ph_);
                ph_ = float(s);
                if (1.0 < s) ph_ = float(s - 2.0);
                v1_ = float(((double(ph_) * double(ph_) * double(depth_) + double(centre_)) - g1) * 0.03125);
                double s2 = double(stereo_) + double(ph_);
                if (1.0 < s2) s2 -= 2.0;
                v2_ = float(((s2 * double(depth_) * s2 + double(centre_)) - g2) * 0.03125);
            }
            g1 += double(v1_);
            const float inL = io[2 * i];
            const float yl = l_.run(inL, g1, fbk_);
            const double ml = double(inL) * dry + double(yl) * wet;
            io[2 * i] = float(ml * dry + l2_.run(ml, g1) * wet);
            g2 += double(v2_);
            const float inR = io[2 * i + 1];
            const float yr = r_.run(inR, g2, fbk_);
            const double mr = double(inR) * dry + double(yr) * wet;
            io[2 * i + 1] = float(mr * dry + r2_.run(mr, g2) * wet);
        }
        g1_ = float(g1);
        g2_ = float(g2);
    }
    // FUN_1002fdb0 has no silence gating: it never reads or clears the flag and always processes.
    void processGated(float* lr, int frames, bool&) override { process(lr, frames); }

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
            case 4: fbk_ = float(double(v) * double(0.97f)); return;
            case 5: stereo_ = float(double(v) + double(v)); return;
            case 6: {
                const double c = double(v) * 29.5;
                centre_ = float((c * c * c + 500.0) * double(invSr_) - 1.0);
                return;
            }
            default: return;
        }
    }
    int count_ = 1;
    float mix_ = 0.5f, inc_ = 0, depth_ = 0, fbk_ = 0, stereo_ = 0, centre_ = 0;
    float g1_ = 0, g2_ = 0, v1_ = 0, v2_ = 0, ph_ = 0;
    Chain l_, r_;
    Chain2 l2_, r2_;
};

}  // namespace

std::unique_ptr<Effect> make_fx_42() { return std::make_unique<BiPhaser>(); }

}  // namespace hs2
