// 72 TalkBox (ctor FUN_1003aa10, vtable 0x100a976c: setSampleRate FUN_1003ab90, tempo FUN_1003ac30,
// process FUN_1003ac50, setParam FUN_1003afc0, paramName 0x1003b170, paramDisplay 0x1003b230).
// Two SVFs per channel (damping 0.07) tuned to the two formants of a vowel; out = x*(1-mix^2) + (lp1 - hp2)*mix/2.
// Every 32 samples: envelope of |L+R| (attack p6 / release p7), triangle LFO (phase in [-1, 1], rate p2:
// exp(p*5.99-3.0)*10*2pi/sr per block), vowel position v = p4*16 + |phase|*(p3-0.5)^3*100
// + (ln(env + 0.1) + 2.3)*(p5-0.5)^3*100, wrapped to [0, 18) and linearly interpolated in the 19-entry formant table
// (ctor constants this+0x80, F1/F2 in Hz); the SVF coefficient is f*2pi/sr (setSampleRate stores 2pi/sr, not 1/sr).
// Sync (p1 > 0.5): phase from the host song position (process param_3/4 as a double: Effect::setSongPos) and the increment from
// the tempo method (0 when never called), so the LFO stays at -0.5.
// The original keeps intermediates on the x87 stack (53-bit precision control); mirrored with double.
// Silence gate: common pattern; a skipped block also clears the |L+R| accumulator (+0x138).
// Oracle-verified (also with fx_compare --gated): bit-exact (-240 dB) over --random 8, all 0 / all 1 and LFO/envelope-heavy sets.
#include "effect_base.h"

namespace hs2 {
namespace {

using X = double;  // the original runs the x87 at 53-bit precision (MSVC default control word)

class TalkBox : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        w_ = float(X(6.2831855f) / X(sr));
        invSr_ = w_;
        ctr_ = 1;
        acc_ = env_ = phase_ = 0.f;
        for (auto& c : ch_) c = Ch{};
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_1003ac30): stores b*32 (never called by the oracle).
    void setTempo(float, float b) override { beat32_ = b * 32.f; }
    // Silence gate: the common pattern, but the skip branch (silent, countdown < 0) also clears the |L+R| accumulator (+0x138).
    void processGated(float* lr, int n, bool& silent) override {
        if (silent && gateCount_ < 0) {
            acc_ = 0.f;
            return;
        }
        EffectBase::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        static const float kF[19][2] = {{300, 900},  {350, 800},  {410, 800},  {540, 850},  {700, 940},
                                        {780, 1090}, {880, 1400}, {650, 1700}, {550, 1750}, {410, 2020},
                                        {300, 2450}, {300, 2020}, {360, 1760}, {440, 1350}, {520, 1030},
                                        {550, 880},  {450, 680},  {300, 770},  {300, 900}};
        X inc = X(rateInc_);
        if (sync_) {
            const X pos = songPos_;  // host song position (process param_3/4, a double)
            phase_ = float(std::fmod(pos * X(note2_), X(2.0)) - X(0.5f));
            inc = X(beat32_) * X(note2_);
        }
        const float dryG = float(X(1.f) - X(mix_) * X(mix_));
        const float wetG = float(X(mix_) * X(0.5f));
        int ctr = ctr_;
        for (int i = 0; i < n; ++i) {
            if (--ctr == 0) {
                const float c = acc_ <= env_ ? rel_ : atk_;
                env_ = float((X(acc_) - X(env_)) * X(c) + X(env_));
                acc_ = 0.f;
                const X ph = inc + X(phase_);
                phase_ = float(ph);
                if (ph > X(1.f)) phase_ = float(ph - X(2.f));
                X a = X(phase_);
                if (a < 0) a = -a;
                X v = (std::log(X(env_) + X(0.1f)) + X(2.3f)) * X(envAmt_) + a * X(lfoAmt_) + X(pos_);
                while (v < 0) v += X(18.f);
                const int iv = int(v);
                const int r = iv % 18;
                const X fr = v - X(iv);
                f1_ = float(((X(kF[r + 1][0]) - X(kF[r][0])) * fr + X(kF[r][0])) * X(w_));
                f2_ = float(((X(kF[r + 1][1]) - X(kF[r][1])) * fr + X(kF[r][1])) * X(w_));
                ctr = 32;
            }
            const X xl = io[2 * i];
            io[2 * i] = svf(ch_[0], xl, dryG, wetG);
            const X xr = io[2 * i + 1];
            io[2 * i + 1] = svf(ch_[1], xr, dryG, wetG);
            acc_ = float(std::fabs(xl + xr) + X(acc_));
        }
        ctr_ = ctr;
    }

private:
    struct Ch {
        float lp1 = 0, bp1 = 0, lp2 = 0, bp2 = 0, hp2 = 0;
    };
    float svf(Ch& s, X x, float dryG, float wetG) const {
        const X l1 = X(s.bp1) * X(f1_) + X(s.lp1);
        s.lp1 = float(l1);
        s.bp1 = float(X(s.bp1) - ((X(s.bp1) * X(0.07f) - x) + l1) * X(f1_));
        const X l2 = X(s.bp2) * X(f2_) + X(s.lp2);
        s.lp2 = float(l2);
        const X h = (X(s.bp2) * X(0.07f) - x) + l2;
        s.hp2 = float(h);
        s.bp2 = float(X(s.bp2) - h * X(f2_));
        return float((X(s.lp1) - X(s.hp2)) * X(wetG) + x * X(dryG));
    }
    static float noteValue(float v) {  // FUN_100489b0
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    static float coef(X p, float w) {
        const X q = X(1.f) - p;
        return float(X(1.f) - std::exp(-((q * q * X(125.f) + X(1.f)) * X(w))));
    }
    void onParam(int i, float v) override {
        const X p = v;
        switch (i) {
            case 0: mix_ = v; return;
            case 1:
            case 2: {
                sync_ = p_[1] > 0.5f;
                const X nv = noteValue(p_[2]);
                note2_ = float(nv + nv);
                const X hz = std::exp(X(p_[2]) * X(5.991464f) - X(2.995732f));
                rateInc_ = float(hz * X(w_) * X(10.f));
                return;
            }
            case 3: {
                const X d = p - X(0.5f);
                lfoAmt_ = float(d * d * d * X(100.f));
                return;
            }
            case 4: pos_ = float(p * X(16.f)); return;
            case 5: {
                const X d = p - X(0.5f);
                envAmt_ = float(d * d * d * X(100.f));
                return;
            }
            case 6: atk_ = coef(p, w_); return;
            case 7: rel_ = coef(p, w_); return;
            default: return;
        }
    }
    Ch ch_[2];
    int ctr_ = 1;
    bool sync_ = false;
    float w_ = 0, mix_ = 0, note2_ = 0, rateInc_ = 0, lfoAmt_ = 0, pos_ = 0, envAmt_ = 0, atk_ = 0, rel_ = 0;
    float beat32_ = 0, acc_ = 0, env_ = 0, phase_ = 0, f1_ = 0, f2_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_72() { return std::make_unique<TalkBox>(); }

}  // namespace hs2
