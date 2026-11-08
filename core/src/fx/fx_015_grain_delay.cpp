// 15 Grain Delay (ctor FUN_1002eb60, setSampleRate FUN_1002ebc0, setParam FUN_1002f300, process FUN_1002ed20,
// tempo FUN_1002ecf0; vtable 0x100a8f8c, paramName 0x1002f460, paramDisplay 0x1002f5a0).
// Mono 3 s line (input L+R plus feedback) read by two overlapping grains A/B with linear crossfade
// (A gain 1 - e^2, B gain e^2, e = grain phase 1 -> 0). At each grain start (e <= 0) B takes over A's position,
// speed and pan, and A restarts: length exp(4.605 p4 + 0.693) ms (2..200 ms), speed 2 - 4 (p5 + 2 (r - 0.5)
// (p6 - 0.5)^2) - 4 (p7 - 0.5)^2 m clamped to +-2 (speed -1 = original pitch; m = sin(LFO phase in [0,1) rad,
// rate exp(6 p8 - 3) Hz) if p7 > 0.5, else 2 x input envelope), start (time + 1.996 r (p9 - 0.5)^2) s ahead of
// the write head (pushed further when the grain would cross it), pan 0.5 + 2 (r - 0.5)(p10 - 0.5). r is MSVC
// rand()/32768 when the respective p > 0.5, else a toggle that flips 0/1 per grain. The wet sum is DC-blocked
// (one-pole, 20/sr), fed back (p3 x 0.999) and mixed (wet 2m - m^2, dry 1 - m^2). p0 mix, p1 sync, p2 time
// (p2^2 s, or 60/bpm / note value halved below 1 s; 0 when the tempo method is never called), p8 also sets the
// envelope follower coefficient. Defaults p2..p10 = 0.5, p5 = 0.75.
// Pitch source (vtable +8 FUN_1002b410, this+0x78 = &part note pitch, kept across setSampleRate): when connected
// and p4 < 0.01, each grain start sets the grain rate to the held note's frequency, 16.3516 Hz * exp(pitch)
// (= 2^(note/12) x C0); while no note is held (pitch <= -9) the previous rate is kept.
// x87 intermediates are mirrored with double (the original runs the FPU at 53-bit precision, MSVC default).
// Oracle-verified: bit-exact (-240 dB floor) over fx_compare --random 8 + extremes 0/1 + hand sets (incl. p4 < 0.01,
// random and toggle modes), with and without --pitch (sets with p4 = 0 / 0.005: the pitch changes the output by
// +3 dB vs no note held).
#include "effect_base.h"
#include "hs2/dsp.h"

namespace hs2 {
namespace {

class GrainDelay : public EffectBase {
public:
    GrainDelay() {
        for (int i = 2; i <= 10; ++i) p_[i] = 0.5f;
        p_[5] = 0.75f;
    }
    void setSampleRate(float sr) override {
        sr_ = sr;
        const double inv = 1.0 / double(sr);
        invSr_ = float(inv);
        posB_ = posA_ = 100.f;
        inc_ = ph_ = -1.f;
        toggle_ = 0;
        w_ = 0;
        spdB_ = spdA_ = panB_ = panA_ = 0;
        hpR_ = hpL_ = 0;
        lfo_ = env_ = 0;
        hpC_ = float(inv * 20.0);
        L_ = int(double(sr) * 3.0);
        buf_.assign(size_t(L_) + 4, 0.f);
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_1002ecf0): seconds per beat = 60 / bpm, then re-applies param 2.
    // Silence gating (process's flag arg, FUN_1002ed20): reset to the tail length while the input is live; while
    // silent count down (return once below 0) with the flag left set, and if the output is still audible
    // (FUN_10025480) reload the countdown with tail + line length (+0xc + this+0x80) and clear the flag.
    void processGated(float* lr, int frames, bool& silent) override {
        if (!silent) {
            gateCount_ = gateLen_;
        } else {
            if (gateCount_ < 0) return;
            gateCount_ -= frames;
        }
        process(lr, frames);
        if (silent && hasSignal(lr, frames)) {
            gateCount_ = gateLen_ + L_;
            silent = false;
        }
    }
    void setPitchSource(const float* p) override { pitch_ = p; }  // FUN_1002b410
    void setTempo(float bpm, float) override {
        beatSec_ = 60.f / bpm;
        setParam(2, p_[2]);
    }
    void process(float* io, int n) override {
        if (buf_.empty()) return;
        const int L = L_;
        const float Lf = float(L);
        const double m2 = double(mix_) * mix_;
        const float dry = float(1.0 - m2), wet = float((double(mix_) + mix_) - m2);
        float* b = buf_.data();
        for (int i = 0; i < n; ++i) {
            float* f = io + 2 * i;
            const float inL = f[0], inR = f[1];
            env_ = float((std::fabs(double(inR) + inL) - env_) * envC_ + env_);
            lfo_ = float(std::fmod(double(rate_) + lfo_, 1.0));
            const double phx = double(ph_) + inc_;
            ph_ = float(phx);
            if (phx <= 0.0) newGrain();
            const float e = float(double(ph_) * ph_);
            // grain A
            int ia = int(posA_);
            const double fa = double(posA_) - double(ia);
            while (ia >= L) ia -= L;
            const double sA = ((double(b[ia + 1]) - b[ia]) * fa + b[ia]) * (1.0 - e) * wet;
            float right = float(sA * panA_);
            double left = sA - right;
            double p = double(spdA_) + posA_;
            posA_ = float(p);
            if (p < 0.0) posA_ = float(p + Lf);
            // grain B
            int ib = int(posB_);
            const double fb = double(posB_) - double(ib);
            while (ib >= L) ib -= L;
            const double sB = ((double(b[ib + 1]) - b[ib]) * fb + b[ib]) * e * wet;
            right = float(sB * panB_ + right);
            left += (1.0 - panB_) * sB;
            p = double(spdB_) + posB_;
            posB_ = float(p);
            if (p < 0.0) posB_ = float(p + Lf);
            // DC blockers, feedback, output
            const double hl = left - hpL_;
            hpL_ = float(hl * hpC_ + hpL_);
            const double hr = double(right) - hpR_;
            hpR_ = float(hr * hpC_ + hpR_);
            b[w_] = float((hr + hl) * fbk_ + inR + inL);
            if (--w_ < 0) {
                w_ = L;
                b[L] = b[0];
                --w_;
            }
            f[0] = float(double(inL) * dry + hl);
            f[1] = float(double(inR) * dry + hr);
        }
        if (env_ < 1e-8f) env_ = 0.f;
        if (std::fabs(hpL_) < 1e-8f) hpL_ = 0.f;
        if (std::fabs(hpR_) < 1e-8f) hpR_ = 0.f;
    }

private:
    void newGrain() {
        ph_ = 1.f;
        if (p_[4] >= 0.01f || !pitch_) {
            inc_ = float(-1.0 / (std::exp(double(p_[4]) * double(4.6051698f) + double(0.69315f)) * sr_ *
                                 double(0.001f)));
        } else if (*pitch_ > -9.f) {
            // fldl2e; fmul (53-bit); f2xm1/fscale; x invSr x -16.351599
            const double x = double((long double)(*pitch_) * 1.442695040888963407359924681001892137L);
            inc_ = float(std::exp2(x) * double(invSr_) * double(-16.351599f));
        }
        // B takes over A
        spdB_ = spdA_;
        panB_ = panA_;
        posB_ = posA_;
        toggle_ = toggle_ == 0 ? 1 : 0;
        // A: speed
        const float u6 = p_[6] - 0.5f;
        double r = u6 > 0.f ? double(rng_->uni()) : double(toggle_);
        double v = (r - 0.5) * u6 * u6;
        const double spd = 2.0 - ((v + v) + p_[5]) * 4.0;
        const float u7 = p_[7] - 0.5f;
        const double mod = u7 > 0.f ? std::sin(double(lfo_)) : double(env_) + env_;
        const double spd2 = spd - double(u7) * u7 * mod * 4.0;
        spdA_ = float(spd2);
        if (spd2 < -2.0) spdA_ = -2.f;
        else if (spd2 > 2.0) spdA_ = 2.f;
        // A: start position
        const float u9 = p_[9] - 0.5f;
        r = u9 > 0.f ? double(rng_->uni()) : double(toggle_);
        const double pos = (r * (double(u9) * u9) * double(1.996f) + time_) * sr_;
        posA_ = float(pos);
        const double x = double(spdA_) / inc_ + pos;
        if (x < 0.0) posA_ = float(pos - x);
        posA_ = float(double(w_) + posA_);
        // A: pan
        const float u10 = p_[10] - 0.5f;
        r = u10 > 0.f ? double(rng_->uni()) : double(toggle_);
        v = (r - 0.5) * u10;
        panA_ = float((v + v) + 0.5);
    }
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2:
                if (p_[1] <= 0.5f) {
                    time_ = float(double(p_[2]) * p_[2] * double(0.999f) + double(0.001f));
                } else {
                    double t = double(beatSec_) / double(noteValue(1.f - p_[2]));
                    while (t > 1.0) t *= 0.5;
                    time_ = float(t);
                }
                return;
            case 3: fbk_ = v * 0.999f; return;
            case 8: {
                rate_ = float(std::exp(double(v) * double(5.991464f) - double(2.995732f)) * double(invSr_));
                const double q = 1.0 - double(v);
                envC_ = float(1.0 - std::exp(-((q * q * 5000.0 + 10.0) * double(invSr_))));
                return;
            }
            default: return;
        }
    }
    // FUN_100489b0: note length in beats, index trunc(v*20.9)
    static float noteValue(float v) {
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    std::vector<float> buf_;
    const float* pitch_ = nullptr;
public:
    void setRandom(MsvcRand* r) override { rng_ = r ? r : &ownRng_; }
private:
    MsvcRand ownRng_;
    MsvcRand* rng_ = &ownRng_;
    int L_ = 0, w_ = 0, toggle_ = 0;
    float mix_ = 0.5f, fbk_ = 0, time_ = 0, beatSec_ = 0, rate_ = 0, envC_ = 0, hpC_ = 0;
    float env_ = 0, lfo_ = 0, ph_ = -1.f, inc_ = -1.f, hpL_ = 0, hpR_ = 0;
    float posA_ = 100.f, posB_ = 100.f, spdA_ = 0, spdB_ = 0, panA_ = 0, panB_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_15() { return std::make_unique<GrainDelay>(); }

}  // namespace hs2
