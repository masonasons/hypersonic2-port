// 71 Wah Pedal (ctor FUN_1003a100, vtable 0x100a96f0: setSampleRate FUN_1003a120, tempo FUN_1003a1f0,
// process FUN_1003a210, setParam FUN_1003a670, paramName 0x1003a820, paramDisplay 0x1003a8f0).
// Per channel a state-variable filter on x/2 (one-pole low-pass lp, coef 8000/sr, feeding a resonant band-pass
// bp/s with frequency f and damping p6), out = x + mix*((lp*(1-mix^2) + 4*bp) - x), mix = p0.
// Every 32 samples: envelope of (L+R)^2 (log(sum + 0.1) + 1.3, instant attack, release p7 or rate-based),
// LFO phase over 64*pi (rate p2: exp(p*5.99-3.0) Hz, scaled by clamp(1 + env*tracking, 0.5, 2)),
// position v by p5 mode (0 auto: env*p3 + p4^2, 1 pedal: p4^2, 2 mod: clamp(-cos(phase), +-0.5)*p3 + p4^2),
// clamped to [0, 1]; f smoothed to (1700 + 13200*v)/sr with coefficient 1500/sr.
// Sync (p1 > 0.5): the phase is taken from the host song position (process param_3/4 as a double: Effect::setSongPos) and the
// per-block increment uses the tempo method's beat value (0 when never called), so the LFO is frozen at
// phase p7-dependent offset (ec - pi).
// The original keeps intermediates on the x87 stack (53-bit precision control); mirrored with double.
// Silence gate: common pattern; a skipped block also clears the envelope (+0xb0).
// Oracle-verified (also with fx_compare --gated): bit-exact (-240 dB) over --random 8, all 0 / all 1 and sets covering all three modes and sync.
#include "effect_base.h"

namespace hs2 {
namespace {

using X = double;  // the original runs the x87 at 53-bit precision (MSVC default control word)

class WahPedal : public EffectBase {
public:
    void setSampleRate(float sr) override {
        sr_ = sr;
        const X inv = X(1.f) / X(sr);
        invSr_ = float(inv);
        ctr_ = 1;
        env_ = phase_ = 0.f;
        lpL_ = bpL_ = sL_ = lpR_ = bpR_ = sR_ = 0.f;
        envAcc_ = 0.f;
        f_ = 0.1f;
        lpC_ = float(X(8000.f) * inv);
        const X lo = X(1700.f) * inv;
        fLo_ = float(lo);
        fRange_ = float(inv * X(14900.f) - lo);
        fSmooth_ = float(inv * X(1500.f));
        for (int i = 0; i < 12; ++i) setParam(i, p_[i]);
    }
    // vtable +0xc (FUN_1003a1f0): stores b*32 (never called by the oracle).
    void setTempo(float, float b) override { beat32_ = b * 32.f; }
    // Silence gate: the common pattern, but the skip branch (silent, countdown < 0) also clears the envelope (+0xb0).
    void processGated(float* lr, int n, bool& silent) override {
        if (silent && gateCount_ < 0) {
            env_ = 0.f;
            return;
        }
        EffectBase::processGated(lr, n, silent);
    }
    void process(float* io, int n) override {
        int ctr = ctr_;
        const float dryK = float(X(1.f) - X(mix_) * X(mix_));
        const float rel = mode_ == 0 ? relC_ : float(X(fSmooth_) * X(0.1f));
        float inc = rateInc_;
        if (sync_) {
            const X pos = songPos_;  // host song position (process param_3/4, a double)
            const X ph = std::fmod(pos * X(noteRad_), X(double(6.2831855f))) - X(3.1415927f);
            phase_ = float(ph);
            inc = float(X(beat32_) * X(noteRad_));
            phase_ = float(ph + X(syncOfs_));
        }
        for (int i = 0; i < n; ++i) {
            if (--ctr == 0) control(rel, inc), ctr = 32;
            const float xl = io[2 * i];
            const float h = float(X(xl) * X(0.5f) - X(lpL_) + X(1e-8f));
            softLimit(bpL_);
            const X lp = X(h) * X(lpC_) + X(lpL_);
            lpL_ = float(lp);
            const X bp = X(f_) * X(sL_) + X(bpL_);
            bpL_ = float(bp);
            sL_ = float(X(sL_) - ((X(damp_) * X(sL_) + bp) + X(h)) * X(f_));
            io[2 * i] = float(((lp * X(dryK) + X(bpL_) * X(4.f)) - X(xl)) * X(mix_) + X(xl));
            const float xr = io[2 * i + 1];
            const X sum = X(xl) + X(xr);
            envAcc_ = float(sum * sum + X(envAcc_));
            const X h2 = X(0.5f) * X(xr) - X(lpR_) + X(1e-8f);
            lpR_ = float(h2 * X(lpC_) + X(lpR_));
            softLimit(bpR_);
            const X bp2 = X(sR_) * X(f_) + X(bpR_);
            bpR_ = float(bp2);
            sR_ = float(X(sR_) - ((X(sR_) * X(damp_) + bp2) + h2) * X(f_));
            io[2 * i + 1] = float(((X(lpR_) * X(dryK) + X(bpR_) * X(4.f)) - X(xr)) * X(mix_) + X(xr));
        }
        ctr_ = ctr;
    }

private:
    static void softLimit(float& b) {
        if (b > 0.f) b = b <= 0.5f ? float(X(b) - X(b) * X(b)) : 0.25f;
    }
    void control(float rel, float inc) {
        static const float kWrap = 201.06194f;  // 64*pi (DLL global @0x103bfdc0)
        const X lvl = std::log(X(envAcc_) + X(0.1f)) + X(1.3f);
        envAcc_ = float(lvl);
        const X e = lvl > X(env_) ? lvl : X(env_) - X(rel) * X(env_);
        env_ = float(e);
        X m = 1.0L;
        if (!sync_) {
            m = X(env_) * X(track_) + X(1.f);
            if (!(m >= X(0.5f))) m = 0.5f;
            else if (m > X(2.f)) m = 2.f;
        }
        const X ph = X(inc) * m + X(phase_);
        phase_ = float(ph);
        if (ph > X(kWrap)) phase_ = float(ph - X(kWrap));
        X v;
        if (mode_ == 0) {
            v = X(env_) * X(depth_) + X(pedal_);
        } else if (mode_ == 1) {
            v = pedal_;
        } else {
            v = -std::cos(X(phase_));
            if (!(v >= X(-0.5f))) v = -0.5f;
            else if (v > X(0.5f)) v = 0.5f;
            v = v * X(depth_) + X(pedal_);
        }
        if (!(v >= X(0))) v = 0;
        else if (v > X(1)) v = 1;
        envAcc_ = 0.f;
        f_ = float(((v * X(fRange_) + X(fLo_)) - X(f_)) * X(fSmooth_) + X(f_));
    }
    static float noteValue(float v) {  // FUN_100489b0
        static const float kNotes[20] = {0.125f, 0.142857f, 0.166667f, 0.2f, 0.25f, 0.333333f, 0.375f,
                                         0.5f,   0.666667f, 0.75f,     1.0f, 1.333333f, 1.5f, 2.0f,
                                         2.6666667f, 3.0f, 4.0f, 5.3333335f, 6.0f, 8.0f};
        const unsigned i = unsigned(int(v * 20.9f));
        return i < 20 ? kNotes[i] : 16.f;
    }
    void onParam(int i, float v) override {
        const X p = v;
        switch (i) {
            case 0: mix_ = v; return;
            case 1:
            case 2: {
                sync_ = p_[1] > 0.5f;
                noteRad_ = float(X(noteValue(p_[2])) * X(6.2831855f));
                const X hz = std::exp(X(p_[2]) * X(5.991464f) - X(2.995732f));
                rateInc_ = float(hz * X(invSr_) * X(201.06194f));
                return;
            }
            case 3: depth_ = float(p * X(3.f) - X(1.5f)); return;
            case 4: pedal_ = float(p * p); return;
            case 5: mode_ = int(p * X(2.9f)); return;
            case 6: damp_ = float(X(0.415f) - p * X(0.4f)); return;
            case 7:
                track_ = float(p * X(3.f) - X(1.5f));
                relC_ = float(X(1.f) - std::exp(-((p * p * X(4000.f) + X(50.f)) * X(invSr_))));
                syncOfs_ = float((p - X(0.5f)) * X(6.2831855f) + X(4.5f));
                return;
            default: return;
        }
    }
    int ctr_ = 1, mode_ = 0;
    bool sync_ = false;
    float mix_ = 0, depth_ = 0, pedal_ = 0, damp_ = 0, track_ = 0, relC_ = 0, syncOfs_ = 0;
    float noteRad_ = 0, rateInc_ = 0, beat32_ = 0;
    float lpC_ = 0, fLo_ = 0, fRange_ = 0, fSmooth_ = 0, f_ = 0.1f;
    float env_ = 0, envAcc_ = 0, phase_ = 0;
    float lpL_ = 0, bpL_ = 0, sL_ = 0, lpR_ = 0, bpR_ = 0, sR_ = 0;
};

}  // namespace

std::unique_ptr<Effect> make_fx_71() { return std::make_unique<WahPedal>(); }

}  // namespace hs2
