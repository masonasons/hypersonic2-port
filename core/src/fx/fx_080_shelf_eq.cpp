// 80 Shelf EQ (ctor FUN_1003b340, vtable 0x100a97dc: setSampleRate FUN_1003b370, process FUN_1003b3f0,
// setParam FUN_1003b7e0, paramName 0x1003b8e0, paramDisplay 0x1003b990; init/tempo 0x10002170 is a no-op).
// Two one-pole filters per channel: y = (x + bassGain*lp(x) + trebleGain*hp(x)) * out.
// p0 output (exp(p*2.7631-1.38155), +-12 dB), p2 bass, p3 treble: below 0.5 the shelf cuts (gain (p-0.5)*1.9 resp.
// *1.8 with a lower corner), above 0.5 it boosts ((p-0.5)^2*12) with a different one-pole coefficient.
// The high-pass is x minus a one-pole low-pass state, taken before the state update.
// The original keeps x, lp and hp on the x87 stack (53-bit precision control); mirrored with double.
// Oracle-verified: bit-exact (-240 dB) over --random 8 --seed 1080, all 0 and all 1.
#include "effect_base.h"

namespace hs2 {
namespace {

// exp(x) as the original computes it: fldl2e; fmul; frndint; f2xm1; fld1; faddp; fscale (53-bit precision control).
double x87exp(double x) {
    const double t = double((long double)x * 1.442695040888963407359924681001892137L);
    const double n = std::nearbyint(t);
    return std::ldexp(std::pow(2.0, t - n), int(n));
}

class ShelfEQ : public EffectBase {
public:
    ShelfEQ() { p_[4] = p_[5] = p_[6] = p_[7] = 0.5f; }
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.5);  // +0xc tail: trunc(sr * 0.5)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        lp_[0] = lp_[1] = hs_[0] = hs_[1] = 0.f;
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    void process(float* io, int n) override {
        const float out = out_;
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                // x87 register values (53-bit): x, lp and hp are never rounded to float before use
                const double x = double(io[2 * i + c]) + double(1e-8f);
                const double lp = (x - double(lp_[c])) * double(bassC_) + double(lp_[c]);
                lp_[c] = float(lp);
                const double hp = x - double(hs_[c]);
                hs_[c] = float(hp * double(trebC_) + double(hs_[c]));
                io[2 * i + c] = float(((hp * double(trebG_) + lp * double(bassG_)) + x) * double(out));
            }
    }

private:
    void onParam(int i, float v) override {
        if (i == 0) {
            out_ = float(x87exp(double(v) * double(2.7631f) - double(1.38155f)));
        } else if (i == 2) {
            const double d = double(v) - 0.5;
            if (0.0 < d) {
                bassC_ = invSr_ * 700.2f;
                bassG_ = float(d * d * 12.0);
            } else {
                bassC_ = invSr_ * 3500.f;
                bassG_ = float(d * double(1.9f));
            }
        } else if (i == 3) {
            const double d = double(v) - 0.5;
            if (0.0 < d) {
                trebC_ = invSr_ * 67000.f;
                trebG_ = float(d * d * 12.0);
            } else {
                trebC_ = invSr_ * 17000.f;
                trebG_ = float(d * double(1.8f));
            }
        }
    }
    float out_ = 1, bassC_ = 0, bassG_ = 0, trebC_ = 0, trebG_ = 0;
    float lp_[2] = {0, 0}, hs_[2] = {0, 0};
};

}  // namespace

std::unique_ptr<Effect> make_fx_80() { return std::make_unique<ShelfEQ>(); }

}  // namespace hs2
