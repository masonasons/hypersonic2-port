// 7 Tuned Resonators (ctor FUN_1002b3b0, setSampleRate FUN_1002b420, process FUN_1002b500, setParam FUN_1002b8c0;
// vtable 0x100a8d34: +8 FUN_1002b410 stores a pitch-source pointer at +0x78, +0xc init 0x10002170 unused).
// Mono input * gain (p6) -> one-pole high-pass (p7) -> up to 7 damped (p8) comb resonators (count 1+int(6.9*p2))
// fed with alternating sign. Base period sr / (440 * 2^((floor(72.9*p3) + p4 - 45.5)/12)), each further comb
// shorter by exp(-0.128*p5) (one step raised a semitone for p5 > 0.9); feedback exp(period * ln(10^(-0.05 *
// exp(6.9 - 9.2*p6) / sr))). Outputs panned across the stereo field by p9 (spread); p0 mix.
// p10 > 0.505 = key tracking: the period scale comes from the pitch source (32*exp(-pitch)), octave-folded so
// that the shortest comb is >= 2 samples and the longest fits the 32 ms line; p10 also sets extra decay used while
// the source reports "no note" (<= -9). The Effect interface has no hook for the pitch source (vtable +8) yet, so
// pitch_ stays null here (setPitchSource is ready for it). Quirk: with tracking on and no pitch source the
// original loops forever (its scale field is never initialised, 0 never doubles); this port starts it at 1.
// The x87 code keeps several values in 53-bit registers; mirrored with double intermediates.
// Oracle-verified: bit-exact (-240 dB floor) over 8 random sets (seeded like --random 8, p10 halved to stay below
// 0.505, since the oracle hangs above that) plus all-0 / defaults / p0..p9 = 1; tracking mode checked separately
// with a pitch source on both sides (scratch harness).
#include "effect_base.h"

namespace hs2 {
namespace {

class TunedResonators : public EffectBase {
public:
    TunedResonators() {
        gate_ = Gate::Detect;  // countdown leaves the flag set; an audible left output (2|x| > 1e-5) reloads/clears
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = 1.f / sr;
        w_ = 0;
        L_ = int(double(sr) * double(0.032f));
        for (auto& b : buf_) b.assign(size_t(L_) + 2, 0.f);
        for (auto& d : damp_) d = 0;
        hp_ = 0;
        reapplyParams();
    }
    // vtable +8 (FUN_1002b410): pointer to the element's pitch value (not reachable through Effect yet).
    void setPitchSource(const float* p) override { pitch_ = p; }
    void process(float* io, int n) override {
        if (L_ <= 0) return;
        const int L = L_, L1 = L - 1;
        double decay = fbLog_;
        bool fold = track_;
        if (pitch_) {
            const float pv = *pitch_;
            if (pv <= -9.f) {
                decay -= noteOff_;
            } else if (track_) {
                cc_ = float(std::exp(-double(pv)) * 32.0);
            }
        }
        if (fold) {
            if (!(cc_ > 0.f)) cc_ = 1.f;  // port guard: the original hangs here (see header)
            while (double(cc_) * double(period_[6]) < 2.0) cc_ = cc_ + cc_;
            while (double(L) < double(cc_) * double(period_[0])) cc_ = cc_ * 0.5f;
        } else {
            cc_ = 1.f;
        }
        int w = w_;
        int tap[7];
        float fb[7];
        for (int k = 0; k < 7; ++k) {
            const double pc = double(period_[k]) * double(cc_);
            int t = int(0.5 + pc) + w;
            if (L1 < t) t -= L;
            tap[k] = t;
            fb[k] = float(std::exp(pc * decay));
        }
        const int cnt1 = count_ + 1;
        float pan0 = 0.5f, inc = 0.f;
        if (cnt1 > 2) {
            pan0 = float(0.5 - double(spread_) * 0.5);
            inc = float(double(spread_) / double(count_ - 1));
        }
        const double mix = mix_, dc = dampCoef_, gain = gain_, hpc = hpCoef_;
        for (int i = 0; i < n; ++i) {
            const float inL = io[2 * i], inR = io[2 * i + 1];
            const double x = (((double(inL) + inR) * gain - hp_) + double(9.99999993922529e-09f));
            const float xf = float(x);
            hp_ = float(x * hpc + hp_);
            double outL = 0, outR = 0;
            int k = 1;
            if (cnt1 > 1) {
                double xs = xf, pan = pan0;
                for (; k < cnt1; ++k) {
                    float* B = buf_[k - 1].data();
                    const double d = (double(B[tap[k - 1]]) - damp_[k - 1]) * dc + damp_[k - 1];
                    damp_[k - 1] = float(d);
                    B[w] = float(d * fb[k - 1] + xs);
                    if (--tap[k - 1] < 0) tap[k - 1] = L1;
                    const double dp = d * pan;
                    outL += dp;
                    outR += d - dp;
                    pan += inc;
                    xs = -xs;
                }
            }
            for (; k < 8; ++k) buf_[k - 1][w] = 0.f;
            if (--w < 0) w = L1;
            io[2 * i] = float((outL - inL) * mix + inL);
            io[2 * i + 1] = float((outR - inR) * mix + inR);
        }
        w_ = w;
    }

private:
    void onParam(int i, float v) override {
        const double dv = v;
        switch (i) {
            case 0: mix_ = v; break;
            case 2: count_ = 1 - int(dv * double(-6.9f)); break;
            case 3:
            case 4:
            case 5: {
                const double e = std::exp(((std::floor(double(p_[3]) * double(72.9f)) + p_[4]) - 45.5) *
                                          double(0.0577622652f));
                const float p0 = float(double(sr_) / (e * 440.0));
                int bits = 0;
                if (double(p_[5]) * double(2.22222f) > 2.0) bits = p_[5] > 0.95f ? 4 : 0x12;
                const double r = std::exp(double(p_[5]) * double(-0.12836047f));
                period_[0] = p0;
                double f = p0;
                for (int k = 1; k < 7; ++k) {
                    f *= r;
                    if (bits & (1 << (k - 1))) f *= double(1.059463f);
                    period_[k] = float(f);
                }
                break;
            }
            case 6: {
                const float t = float(std::exp(double(6.9f) - dv * double(9.2f)));
                gain_ = float(std::exp(double(-1.693f) - dv * double(2.2f)));
                fbLog_ = float(std::log(std::pow(10.0, double(t) * double(invSr_) * double(-0.05f))));
                break;
            }
            case 7: hpCoef_ = float(dv * double(invSr_) * dv * 12000.0); break;
            case 8: dampCoef_ = float(1.0 - std::exp(-((dv * dv * dv * 300000.0 + 10000.0) * double(invSr_)))); break;
            case 9: spread_ = v; break;
            case 10: {
                track_ = v > 0.505f;
                const double u = dv - 0.5;
                noteOff_ = float(u * u * u * u * double(0.04f));
                break;
            }
            default: break;
        }
    }
    const float* pitch_ = nullptr;
    int L_ = 0, w_ = 0, count_ = 1;
    bool track_ = false;
    std::vector<float> buf_[7];
    float period_[7] = {}, damp_[7] = {};
    float mix_ = 0.5f, gain_ = 0, hpCoef_ = 0, dampCoef_ = 0, fbLog_ = 0, spread_ = 0, noteOff_ = 0;
    float hp_ = 0, cc_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_7() { return std::make_unique<TunedResonators>(); }

}  // namespace hs2
