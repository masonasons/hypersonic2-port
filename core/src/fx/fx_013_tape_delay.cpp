// 13 Tape Delay (ctor FUN_1002e260, setSampleRate FUN_1002e2b0, setParam FUN_10043a90, process FUN_1002e3c0,
// retap FUN_1002e780, tempo FUN_10043a60; vtable 0x100a8f14, paramName 0x1002e8a0, paramDisplay 0x1002e9f0).
// Mono 3.01 s line with four playback heads. p0 mix, p1 sync, p2 time (0.05..3 s, or note value x beat; the
// beat defaults to 22050 samples when the tempo method is never called), p3 feedback, p4 "vintage", p5..p8 head
// 1..4 positions (0.125..1 x time), p9 pan 3+4, p10 vol 3+4.
// Heads 3+4 are summed (feedback source) and panned (p9) onto the outputs, added (x 2 p10^2) to head 2 (left) and
// head 1 (right). Feedback: the sum goes through a peak limiter (gain envelope floored at 1, |x| * 3), x feedback
// p3^2/4 x (1 + p4^2), then a high-pass (5000 p4 rad/s) and low-pass (57000 - 50000 p4) written into a 256-sample
// "wow" ring buffer that is read back with a fractional reader re-targeted every 256 samples to a random delay
// (MSVC rand, depth p4^2 x sr/8000 samples) before entering the tape line. The x87 intermediates are
// mirrored with double (the original runs the FPU at 53-bit precision, MSVC default; so does the oracle).
// Oracle-verified: bit-exact (-240 dB floor) over fx_compare --random 8 + extremes 0/1 + hand sets.
#include "effect_base.h"
#include "hs2/dsp.h"

namespace hs2 {
namespace {

using LD = double;  // x87 intermediates (MSVC runs the FPU at 53-bit precision)

class TapeDelay : public EffectBase {
public:
    void setSampleRate(float sr) override {
        gate_ = Gate::Detect;  // keeps running while its output is audible (FUN_10025480)
        sr_ = sr;
        invSr_ = 1.f / sr;
        w_ = 0;
        hp_ = lp_ = 0;
        env_ = 1.f;
        rd_ = wwr_ = 0;
        frac_ = 0;
        wow_.assign(258, 0.f);
        wowMax_ = sr * 0.000125f;
        L_ = int(double(sr) * double(3.01f));
        buf_.assign(size_t(L_), 0.f);
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_10043a60): beat = 1/b; in sync mode the time is re-derived.
    void setTempo(float, float b) override {
        beat_ = 1.f / b;
        if (sync_) {
            time_ = float(LD(1.f / b) * LD(invNote_));
            retap();
        }
    }
    void process(float* io, int n) override {
        if (buf_.empty()) return;
        const int L = L_, L1 = L - 1;
        int w = w_;
        int r0 = tap_[0] + w, r1 = tap_[1] + w, r2 = tap_[2] + w, r3 = tap_[3] + w;
        while (r0 > L1) r0 -= L;
        while (r1 > L1) r1 -= L;
        while (r2 > L1) r2 -= L;
        while (r3 > L1) r3 -= L;
        const float dry = float(1.0 - LD(mix_) * mix_);
        const LD g = LD(p_[4]) * p_[4] + 1.0;
        const float fbg = float(g * fbk_);
        const float wetg = float(g * mix_ * 0.25);
        float* b = buf_.data();
        float* wb = wow_.data();
        for (int i = 0; i < n; ++i) {
            float* f = io + 2 * i;
            --r3;
            const float a = b[r3 + 1];
            if (r3 < 0) r3 = L1;
            const float c = b[r2];
            if (--r2 < 0) r2 = L1;
            const LD sum = LD(c) + a;
            --r1;
            const LD x = (LD(c) - a) * pan_ + a;
            const float diff = float(sum - x);
            const LD y1 = x * vol_ + b[r1 + 1];
            if (r1 < 0) r1 = L1;
            --r0;
            const LD y0 = LD(diff) * vol_ + b[r0 + 1];
            if (r0 < 0) r0 = L1;
            const float inL = f[0], inR = f[1];
            f[0] = float(y1 * wetg + LD(inL) * dry);
            f[1] = float(y0 * wetg + LD(inR) * dry);
            // feedback: limiter, gain, high-pass, low-pass
            LD t = std::fabs(sum) * 3.0;
            if (!(t > env_)) t = 1.0;
            const LD env = env_ - (env_ - t) * rel_;
            env_ = float(env);
            const LD fb = sum / env * fbg;
            const LD h = ((LD(inR) + fb) + inL) - hp_;
            hp_ = float(h * hpC_ + hp_);
            lp_ = float(((h + fb) - lp_) * lpC_ + lp_);
            wb[wwr_] = lp_;
            if (--wwr_ < 0) {  // every 256 samples: new random wow target
                wwr_ = 255;
                wb[256] = wb[0];
                const float base = float(LD(rd_) + frac_);
                const LD v = (LD(rng_->next()) * LD(6.1035156e-05f) - 1.0) * wowDepth_ + base;
                float tgt = float(v);
                if (v < 1.0) tgt = 1.f;
                else if (v > wowMax_) tgt = wowMax_;
                vel_ = float((LD(tgt) - base) * 0.00390625 - 1.0);
            }
            const LD fr = LD(vel_) + frac_;
            frac_ = float(fr);
            if (fr < 0.0) {
                LD q = frac_;
                int k = rd_;
                do {
                    q += 1.0;
                    k = (k - 1) & 0xff;
                } while (q < 0.0);
                frac_ = float(q);
                rd_ = k;
            }
            const float v0 = wb[rd_], v1 = wb[rd_ + 1];
            --w;
            b[w + 1] = float((LD(v1) - v0) * frac_ + v0);
            if (w < 0) w = L1;
        }
        w_ = w;
    }

private:
    void onParam(int i, float v) override {
        switch (i) {
            case 0: mix_ = v; return;
            case 1: case 2: {
                sync_ = 0.5f < p_[1];
                const LD inv = 1.0 / LD(noteValue(1.f - p_[2]));
                invNote_ = float(inv);
                if (sync_)
                    time_ = float(inv * beat_);
                else
                    time_ = float((LD(p_[2]) * p_[2] * LD(2.95f) + LD(0.05f)) * sr_);
                retap();
                return;
            }
            case 3: fbk_ = v * v * 0.25f; return;
            case 4:
                lpC_ = float(1.0 - std::exp(-((57000.0 - double(v) * 50000.0) * double(invSr_))));
                hpC_ = float(1.0 - std::exp(double(v) * double(invSr_) * -5000.0));
                rel_ = float(1.0 - std::exp(-(std::exp(double(v) * double(6.9f)) * double(invSr_))));
                wowDepth_ = float(LD(v) * wowMax_ * v);
                return;
            default: retap(); return;
        }
    }
    void retap() {  // FUN_1002e780
        LD t = time_;
        while (!(t < LD(L_))) t *= 0.5;
        for (int k = 0; k < 4; ++k) tap_[k] = int((LD(p_[5 + k]) * LD(0.875f) + LD(0.125f)) * t);
        pan_ = 0.5f - p_[9] * 0.5f;
        const float q = p_[10] * p_[10];
        vol_ = q + q;
    }
    // FUN_100489b0: note length in beats, index trunc(v*20.9)
    static float noteValue(float v) {
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    std::vector<float> buf_, wow_;
public:
    void setRandom(MsvcRand* r) override { rng_ = r ? r : &ownRng_; }
private:
    MsvcRand ownRng_;
    MsvcRand* rng_ = &ownRng_;
    int L_ = 1, w_ = 0, tap_[4] = {0, 0, 0, 0}, rd_ = 0, wwr_ = 0;
    bool sync_ = false;
    float time_ = 4410.f, beat_ = 22050.f, invNote_ = 0;
    float mix_ = 0.5f, fbk_ = 0, pan_ = 0, vol_ = 0, lpC_ = 0, hpC_ = 0, rel_ = 0, wowDepth_ = 0, wowMax_ = 0;
    float hp_ = 0, lp_ = 0, env_ = 1, frac_ = 0, vel_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_13() { return std::make_unique<TapeDelay>(); }

}  // namespace hs2
