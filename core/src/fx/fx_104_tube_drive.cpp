// 104 Tube Drive (ctor FUN_100406d0, setSampleRate FUN_10040720, setParam FUN_10040da0, process FUN_100407c0).
// v = float(x*gain + bias); hard clip to +-2 by float bit masking (bit 30 set -> keep sign+bit 30 only);
// y = soft ? (v - v^3/12)*0.1875 : v*0.125. Then h += ((y - x) - h)*lo, l += ((y - h) - l)*hi, out = l mixed with x.
// Gain p2^3*100+8 glides per sample: step = (target - gain)*0.0008 recomputed each block (snap within 1e-5).
// Mono mode (p7 <= 0.5) processes the LEFT input only and writes it to both outputs.
// p0 mix (ctor default 1), p2 drive (0), p3 clip soft/hard (0), p4 bias -2p, p5 hi cut (p*34000+10000)/sr,
// p6 lo cut (p^2*10000+20)/sr, p7 mono/stereo. setSampleRate seeds the h states with bias/4.
// Quirk kept: only the left-channel states are flushed below 1e-8 at the end of a block.
// Silence gating: standard countdown pattern, but a skipped block snaps the gain glide to its target.
// Oracle-verified: bit-exact (-240 dB) over --random 8 + extremes; --gated: -240 dB, 0 flag mismatches.
#include "effect_base.h"

namespace hs2 {
namespace {

class TubeDrive : public EffectBase {
public:
    TubeDrive() {
        p_[0] = 1.f;
        p_[2] = 0.f;
        p_[3] = 0.f;
        p_[4] = p_[5] = p_[6] = 0.5f;
    }
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        reapplyParams();
        gain_ = target_;
        h_[0] = h_[1] = float(double(bias_) * 0.25);
        l_[0] = l_[1] = 0;
    }
    // Standard gate pattern, except that a skipped block (silent, countdown < 0) snaps the gain glide to its target.
    void processGated(float* lr, int frames, bool& silent) override {
        if (!silent) {
            gateCount_ = gateLen_;
        } else {
            if (gateCount_ < 0) {
                gain_ = target_;
                return;
            }
            gateCount_ -= frames;
            silent = false;
        }
        process(lr, frames);
    }
    void process(float* io, int n) override {
        float step = 0.f;
        if (gain_ != target_) {
            const double f = n <= 0x4e2 ? double(0.0008f) : 1.0 / double(n);
            step = float((double(target_) - gain_) * f);
        }
        for (int i = 0; i < n; ++i) {
            float* f = io + 2 * i;
            if (!stereo_) {
                const float o = shape(f[0], 0);
                gain_ = float(double(step) + gain_);
                f[0] = f[1] = o;
            } else {
                f[0] = shape(f[0], 0);
                const double y = drive(f[1]);
                gain_ = float(double(step) + gain_);
                f[1] = filter(y, f[1], 1);
            }
        }
        if (std::fabs(double(target_) - gain_) < double(1e-05f)) gain_ = target_;
        if (std::fabs(h_[0]) < 1e-08f) h_[0] = 0;
        if (std::fabs(l_[0]) < 1e-08f) l_[0] = 0;
    }

private:
    double drive(float x) const {
        float v = float(double(x) * gain_ + bias_);
        unsigned u;
        std::memcpy(&u, &v, 4);
        if (u & 0x40000000u) {
            u &= 0xc0000000u;
            std::memcpy(&v, &u, 4);
        }
        if (soft_) return (double(v) - double(v) * v * v * double(0.0833333358f)) * double(0.1875f);
        return double(v) * 0.125;
    }
    float filter(double y, float x, int c) {
        const double h = ((y - x) - h_[c]) * lo_ + h_[c];
        h_[c] = float(h);
        const double l = ((y - h) - l_[c]) * hi_ + l_[c];
        l_[c] = float(l);
        return float((l - x) * mix_ + x);
    }
    float shape(float x, int c) { return filter(drive(x), x, c); }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 2: target_ = float(double(v) * v * v * 100.0 + 8.0); return;
            case 3: soft_ = v > 0.5f; return;
            case 4: bias_ = float(double(v) * -2.0); return;
            case 5: hi_ = float((double(v) * 34000.0 + 10000.0) * invSr_); return;
            case 6: lo_ = float((double(v) * v * 10000.0 + 20.0) * invSr_); return;
            case 7: stereo_ = v > 0.5f; return;
            default: return;
        }
    }
    bool soft_ = false, stereo_ = false;
    float mix_ = 1, target_ = 8, gain_ = 0, bias_ = 0, hi_ = 0, lo_ = 0;
    float h_[2] = {0, 0}, l_[2] = {0, 0};
};

}  // namespace

std::unique_ptr<Effect> make_fx_104() { return std::make_unique<TubeDrive>(); }

}  // namespace hs2
