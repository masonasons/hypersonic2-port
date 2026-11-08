// 70 Vintage Filter (ctor FUN_10039140, vtable 0x100a9610: setSampleRate FUN_10039160, process FUN_10039220,
// setParam FUN_100399e0, paramName 0x10039c40, paramDisplay 0x10039df0; init/tempo 0x10002170 is a no-op).
// A 4-stage ladder (each stage: s += ((in + in_prev) - 2s) * k) with soft-clipped feedback
// fb = (p - p^3/12) * res * 1.0576, p = last stage clamped to +-2 by masking the float bits (|p| >= 2 -> +-2).
// Input is scaled by 4, output = in*4*(0.25 - 0.25*mix) + 0.25*mix*tap[mode] (mode p4: LP4, LP3, LP2, LP1, BP, HP).
// p11 <= 0.5: mono, the left filter runs twice per sample (k clamp 0.5) and also feeds the right output;
// p11 > 0.5: stereo, cutoff one octave higher, k clamp 0.85.
// Control rate (every 16 samples): log cutoff smoothed 1%, envelope (p8 mode: 0 follower, 1/2 trigger AD, 3/4 follower
// that also scales the LFO rate), LFO (p5 rate, p6 depth: >= 0.5 sine, < 0.5 sample and hold via rand() at each wrap),
// k and resonance from exp(...).
// Note pitch (vtable +8 FUN_1002b410 stores a pointer at this+0x78 to the part's note pitch, -10 when no note is held):
// its only use is the AD envelope state, checked at the start of each process block: pitch < -9 -> state 2 (release);
// pitch >= -9 while in state 2 -> state 0 (attack; modes 1/2 attack towards 2, mode 1 switches to decay above 1.5).
// A legato note change does not retrigger, and since setSampleRate puts the state at 1 (decay), a note already held
// at start does not trigger either: the first attack follows the first note-off/note-on pair (as in the original).
// The original keeps intermediates on the x87 stack (53-bit precision control); mirrored with double.
// Silence gate: common pattern, but a block entered with the silent flag set runs without the +0.012 resonance gain.
// Oracle-verified (also with fx_compare --gated): bit-exact (-240 dB) with and without --pitch, over --random 8, all 0 / all 1 and sets covering every
// env mode (0-4, mono and stereo), S&H and both attack/release extremes.
#include "effect_base.h"
#include "hs2/dsp.h"

namespace hs2 {
namespace {

using X = double;  // the original runs the x87 at 53-bit precision (MSVC default control word)

class VintageFilter : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        invSr_ = float(1.0 / X(sr));
        ctr_ = 1;
        for (auto& c : st_)
            for (auto& v : c) v = 0.f;
        envAcc_ = env_ = sh_ = phase_ = 0.f;
        trig_ = 1;
        lnSr_ = float(std::log(X(sr)));
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
        cutCur_ = cutTarget_;
    }
    void setPitchSource(const float* p) override { pitch_ = p; }
    // Silence gate: common pattern, but the resonance gain only gets its +0.012 when the flag was clear on entry.
    void processGated(float* lr, int n, bool& silent) override {
        if (!silent) {
            gateCount_ = gateLen_;
            run(lr, n, float(X(reso_) + X(0.012f)));
        } else {
            if (gateCount_ < 0) return;
            gateCount_ -= n;
            silent = false;
            run(lr, n, reso_);
        }
    }
    void process(float* io, int n) override { run(io, n, float(X(reso_) + X(0.012f))); }

private:
    void run(float* io, int n, float resGain) {
        if (pitch_) {
            if (*pitch_ < -9.f) trig_ = 2;
            else if (trig_ == 2) trig_ = 0;
        }
        const bool mono = mono_ == 1;
        const float wetG = float(X(mix_) * X(0.25f));
        const float dryG = float(X(0.25f) - X(wetG));
        const float clampK = mono ? 0.5f : 0.85f;
        if (mono) st_[1][0] = st_[1][1] = st_[1][2] = st_[1][3] = st_[1][4] = 0.f;
        const int tap = 1 + tapMode_;
        for (int i = 0; i < n; ++i) {
            if (--ctr_ == 0) control(resGain, clampK);
            const X xl = X(io[2 * i]) * X(4.f);
            envAcc_ = float(xl * xl + X(envAcc_));
            X s80 = stage(st_[0], xl, float(X(st_[0][1]) + X(1e-8f)));
            if (mono) s80 = stage(st_[0], xl, float(s80));
            st_[0][5] = float(s80 - X(st_[0][3]));
            st_[0][6] = float(X(st_[0][4]) - X(st_[0][0]));
            io[2 * i] = float(X(wetG) * X(st_[0][tap]) + xl * X(dryG));
            const X xr = X(io[2 * i + 1]) * X(4.f);
            envAcc_ = float(xr * xr + X(envAcc_));
            float wet;
            if (mono) {
                wet = st_[0][tap];
            } else {
                const X r80 = stage(st_[1], xr, float(X(st_[1][1]) + X(1e-8f)));
                st_[1][5] = float(r80 - X(st_[1][3]));
                st_[1][6] = float(X(st_[1][4]) - X(st_[1][0]));
                wet = st_[1][tap];
            }
            io[2 * i + 1] = float(X(wetG) * X(wet) + xr * X(dryG));
        }
        if (env_ < 1e-8f) env_ = 0.f;
    }

private:
    // s[0] previous input, s[1..4] = stages 4..1 (this+0x80,0x84,0x88,0x8c), s[5] BP, s[6] HP.
    X stage(float* s, X x4, float pin) {
        uint32_t b;
        std::memcpy(&b, &pin, 4);
        if (b & 0x40000000u) b &= 0xc0000000u;
        float p;
        std::memcpy(&p, &b, 4);
        const X fb = (X(p) - X(p) * X(p) * X(p) * X(0.083333f)) * X(res_) * X(1.0576f);
        const X u = x4 - fb;
        const float old4 = s[4], old3 = s[3], old2 = s[2];
        s[4] = float(((u + X(s[0])) - X(s[4]) - X(s[4])) * X(k_) + X(s[4]));
        s[0] = float(u);
        s[3] = float(((X(old4) + X(s[4])) - X(s[3]) - X(s[3])) * X(k_) + X(s[3]));
        const X n2 = ((X(old3) + X(s[3])) - X(s[2]) - X(s[2])) * X(k_) + X(s[2]);
        s[2] = float(n2);
        const X n1 = ((n2 + X(old2)) - X(s[1]) - X(s[1])) * X(k_) + X(s[1]);
        s[1] = float(n1);
        return n1;
    }
    static X ex(X v) { return std::exp(v); }
    void control(float resGain, float clampK) {
        ctr_ = 16;
        cutCur_ = float((X(cutTarget_) - X(cutCur_)) * X(0.01f) + X(cutCur_));
        X inc = lfoInc_;
        const float lvl = float(std::log(X(envAcc_) + X(1.f)));
        envAcc_ = lvl;
        bool follow = false;
        switch (envMode_) {
            case 1:
                if (trig_ != 0) {
                    env_ = float(X(env_) - X(envRel_) * X(env_));
                } else {
                    const X e = (X(2.f) - X(env_)) * X(envAtk_) + X(env_);
                    env_ = float(e);
                    if (e > X(1.5f)) trig_ = 1;
                }
                break;
            case 2:
                if (trig_ != 0)
                    env_ = float((X(1e-8f) - X(env_)) * X(envRel_) + X(env_));
                else
                    env_ = float((X(2.f) - X(env_)) * X(envAtk_) + X(env_));
                break;
            case 3:
            case 4:
                inc = ex((X(env_) - X(1.f)) * std::fabs(X(envDepth_))) * inc;
                follow = true;
                break;
            default: follow = true; break;
        }
        if (follow) {
            const float c = lvl > env_ ? envAtk_ : envRel_;
            env_ = float((X(lvl) - X(env_)) * X(c) + X(env_));
        }
        envAcc_ = 0.f;
        const float envAmt = envMode_ == 3 ? 0.f : float(X(envDepth_) * X(env_));
        const X ph = inc + X(phase_);
        phase_ = float(ph);
        if (ph > X(6.2831855f)) {
            phase_ = float(std::fmod(X(phase_), X(double(6.2831855f))));
            const X r = X(rng_->next()) * X(3.0517578e-05f);
            sh_ = sh_ >= 0.f ? float(-r) : float(r);
        }
        const X lfo = sampleHold_ ? X(sh_) : std::sin(X(phase_));
        const X g = ex(lfo * X(lfoDepth_) + X(cutCur_) + X(envAmt));
        float gf = float(g);
        if (g > X(clampK)) gf = clampK;
        const X d = X(gf) - X(0.366f);
        const X d2 = d * d;
        const X om = X(1.f) - X(gf);
        k_ = float((X(0.8f) * om + X(1.f)) * X(gf));
        res_ = float(((d2 * X(20.f) + X(7.f)) * d2 + X(2.5f)) * om * X(resGain));
    }
    void onParam(int i, float v) override {
        const X p = v;
        switch (i) {
            case 0: mix_ = v; return;
            case 2:
            case 11:
                mono_ = p_[11] <= 0.5f ? 1 : 0;
                cutTarget_ = float(((X(p_[2]) * X(6.9f) + X(4.f)) - X(lnSr_)) + (mono_ ? X(0) : X(0.6931471f)));
                return;
            case 3: reso_ = float(p * X(0.988f)); return;
            case 4: tapMode_ = int(p * X(5.9f)); return;
            case 5: lfoInc_ = float(ex(p * X(8.29832f) - X(3.f)) * X(invSr_) * X(100.53097f)); return;
            case 6: {
                sampleHold_ = v < 0.5f;
                const X d = p - X(0.5f);
                lfoDepth_ = float(d * d * d * X(32.f));
                return;
            }
            case 7: {
                const X d = p - X(0.5f);
                envDepth_ = float(d * d * d * X(32.f));
                return;
            }
            case 8: envMode_ = int(p * X(4.9f)); return;
            case 9: envAtk_ = float(X(1.f) - ex(-(ex(X(7.5f) - p * X(6.f)) * X(invSr_)))); return;
            case 10: envRel_ = float(X(1.f) - ex(-(ex(X(7.f) - p * X(6.f)) * X(invSr_)))); return;
            default: return;
        }
    }
    float st_[2][7] = {};
    float k_ = 0, res_ = 0, envAcc_ = 0, env_ = 0, sh_ = 0, phase_ = 0, lnSr_ = 0;
    float cutTarget_ = 0, cutCur_ = 0, reso_ = 0, mix_ = 0, lfoInc_ = 0, lfoDepth_ = 0, envDepth_ = 0;
    float envAtk_ = 0, envRel_ = 0;
    int ctr_ = 1, trig_ = 1, mono_ = 1, tapMode_ = 0, envMode_ = 0;
    bool sampleHold_ = false;
public:
    void setRandom(MsvcRand* r) override { rng_ = r ? r : &ownRng_; }
private:
    MsvcRand ownRng_;
    MsvcRand* rng_ = &ownRng_;
    const float* pitch_ = nullptr;
};

}  // namespace

std::unique_ptr<Effect> make_fx_70() { return std::make_unique<VintageFilter>(); }

}  // namespace hs2
