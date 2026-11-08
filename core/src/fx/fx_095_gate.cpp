// 95 Gate (ctor FUN_1003eb50, setSampleRate FUN_1003eb70, setParam FUN_1003f080, process FUN_1003ec10).
// Linked-stereo peak follower env = max(|L|,|R|), else env*(1 - 0.2*release). Above the threshold
// exp(4.6*p2 - 4.6) the 50 ms hold counter is re-armed; below 0.7*threshold it counts down. While the hold is
// non-zero the gain glides to "open" = exp(4.6*p0 - 2.3) with the attack coefficient, else to
// "closed" = (open + 1e-10) - open*p3 with the release coefficient. Attack 1-exp(-exp(8-7*p4)/sr),
// release 1-exp(-exp(7-8*p5)/sr). Quirk kept: L is multiplied by the x87 gain, R by its float copy.
// Oracle-verified: bit-exact (-240 dB) over --random 8 + extremes.
#include "effect_base.h"

namespace hs2 {
namespace {

class Gate : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gateLen_ = int(double(sr) * 0.1);  // +0xc tail: trunc(sr * 0.1)
        sr_ = sr;
        invSr_ = float(1.0 / double(sr));
        gain_ = env_ = 0;
        count_ = 0;
        hold_ = int(double(sr) * double(0.05f));
        reapplyParams();
    }
    void process(float* io, int n) override {
        const float dec = float(1.0 - double(rel_) * double(0.2f));
        for (int i = 0; i < n; ++i) {
            const float L = io[2 * i], R = io[2 * i + 1];
            double m = std::fabs(L);
            if (std::fabs(R) > m) m = std::fabs(R);
            if (!(m > double(env_))) m = double(dec) * env_;
            env_ = float(m);
            if (env_ > thr_) count_ = hold_;
            else if (!(double(thr_) * double(0.7f) <= double(env_)) && count_ != 0) --count_;
            double g;
            if (count_ != 0) g = (double(open_) - gain_) * atk_ + gain_;
            else g = (double(closed_) - gain_) * rel_ + gain_;
            gain_ = float(g);
            io[2 * i] = float(g * L);
            io[2 * i + 1] = float(double(R) * gain_);
        }
    }

private:
    static double x87exp(double a) { return std::exp(a); }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: case 3: {
                const double ex = x87exp(double(p_[0]) * double(4.6f) - double(2.3f));
                open_ = float(ex);
                closed_ = float((double(1e-10f) + ex) - ex * p_[3]);
                return;
            }
            case 2: thr_ = float(x87exp(double(v) * double(4.6f) - double(4.6f))); return;
            case 4: atk_ = float(1.0 - x87exp(-(x87exp(8.0 - double(v) * 7.0) * invSr_))); return;
            case 5: rel_ = float(1.0 - x87exp(-(x87exp(7.0 - double(v) * 8.0) * invSr_))); return;
            default: return;
        }
    }
    int count_ = 0, hold_ = 0;
    float env_ = 0, gain_ = 0, open_ = 1, closed_ = 0, thr_ = 0, atk_ = 0, rel_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_95() { return std::make_unique<Gate>(); }

}  // namespace hs2
