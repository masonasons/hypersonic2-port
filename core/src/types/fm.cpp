// FM element (type 4).
// Original: voice render mode 4 = FUN_1004c920 (part render switch FUN_100139c0); voice note-on FUN_10014910 case 4
// (@10015ee2..1001625b); element setter FUN_100221f0 cases 0x92..0xa4 (per-type params 0x11..0x23, docs/engine/
// element_setter.c); coarse ratio curve FUN_10049a00; param names docs/params.json "element_per_type"["4"].
//
// Algorithm (two modulators, one carrier, the shared filter/amp stage):
//   modulator i: 23-bit integer phase, inc_i = trunc(ratio_i * carrierInc) per control block, ratio_i = 2^22 *
//     (coarse + fine'), fine' = fine * 2^((60 - key)/12) when coarse < 2.1 and fine < 0.02 (a fixed-Hz offset);
//     modulator 2 is also key-scaled by 2^((60 - key) * (1 - Keytrack)/12).
//     waveform: s = asfloat(phase + 0x40000000) - 3 (a saw in [-1, 1)), then s - s^3.
//   carrier phase (period 2, in [-1, 1]): x = (s1*d1 + s2*d2) * index + phase + inc, wrapped; the carrier is the
//     polynomial sine ((x^2/2 - 3/2) x^2 + 1) x, plus s1 * Thru Mix. Then the shared SVF + one-pole, gain ramps.
//   d1 / thru: Depth / Thru Mix scaled by key (Lo/Hi Scale around Breakpoint, at most 24 semitones' worth of
//     exp(0.0577622 y)) and velocity ((vn^2 - 1/2) * VelSens + 1/2, vn = (vel + 1)/2); d2 the same for modulator 2,
//     and d2 glides per block toward Env Level * d2 (or from (2 - EL) * d2 up to d2 when EL > 1), coefficient
//     timeCoef(Env Rate) * exp((1/2 - Vel->Decay) * vel * 8), at most 1.
//   index = exp(3 (min(Wave, 1) - 1)) per block (the Wave destination: param 0x62 + key/vel/random + matrix).
//   LF Carrier > 0: the carrier increment is fixed at LFC * (1 + rnd * RandomPhase) / sr (modulators still follow
//     the key).
//   Note-on phase: x0 = rnd * RandomPhase / 2 + Phase - VelToPhase * vel; modulators start at trunc(x0 * 8388607),
//     the carrier at x0 / 2.
// The x87 runs at 53 bits: register values are double, float stores happen where the original stores.
//
// The shared Voice (voice.cpp) supplies the x87-exact note-on, control update, envelopes, matrix and LFO 3; this file
// only adds the FM case of the note-on (oscillator start state) and the FM render loop. No note-on matrix pass
// for FM (FUN_10049dd0 runs only in the Sample case).
// Verification (part_oracle vs hs2render --dry, 2026-11-08): test tones 43/1, 43/5, 43/6, 43/7, 36/36 and ~30 factory
// FM elements (single notes, 3-note chords, keys 24..100, velocities 10..127, click, glide, LF carrier, note delay):
// per-block voice state bit-identical to the original; audio -114..-147 dB outside the part render's 1e-6 bias frame.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "hs2/element_types.h"

namespace hs2 {

namespace {

// FUN_10049a00: coarse ratio = floor(v <= 0.5 ? 24 v : exp(3.378 v + 0.796)) / 2
float coarseRatio(float v) {
    const double r = v <= 0.5f ? double(v) * 24.0 : x87exp(double(v) * double(3.378f) + double(0.796f));
    return float(std::floor(r) * 0.5);
}

struct FMParams : TypeParams {
    float coarse1 = 0, fine1 = 0, thru = 0, depth1 = 0, velSens1 = 0, lo1 = 0, bp1 = 0, hi1 = 0;  // +0x8d8..0x8f4
    float coarse2 = 0, fine2 = 0, keytrack2 = 0, depth2 = 0, velSens2 = 0, lo2 = 0, bp2 = 0, hi2 = 0;  // +0x8f8..0x914
    float envRate = 1, envLevel = 0, lfCarrier = -1;  // +0x918, +0x91c, +0x920
    float velToDecay = 0;  // +0xa8: raw stored param 0x24 (Vel->Decay)
    float invSr = 0;       // part+0x4c4 = 1/sr
};

class FMVoice : public Voice {
protected:
    const FMParams& fp() const { return static_cast<const FMParams&>(*e_->typeParams); }

    void startOsc(const VoiceStart& vs, float, MsvcRand& rng) override {  // FUN_10014910 @10015ee2
        const FMParams& P = fp();
        const int N = vs.note + vs.transpose;
        const double vel = vel_;
        rateLn_ = float(double(c_.lnRef) + double(0.6931471228599548f));  // elem+0x14 + ln2
        maxInc_ = 1.f;
        {
            const double x = (double(rng.bi()) * e_->randStart * 0.5 + e_->start) - double(e_->velToStart) * vel;
            const int ph = int(x * 8388607.0);  // _ftol: truncation
            ph1_ = ph2_ = uint32_t(ph);
            phase_ = float(double(float(x)) * 0.5);
        }
        const double semiKey = double(60 - N) * double(kSemitone);
        {
            double f = P.fine1;
            if (P.coarse1 < 2.1f && P.fine1 < 0.02f) f *= x87exp(semiKey);
            ratio1_ = float((f + P.coarse1) * 4194304.0);
        }
        {
            double f = P.fine2;
            if (P.coarse2 < 2.1f && P.fine2 < 0.02f) f *= x87exp(semiKey);
            const double kt = x87exp(double(60 - N) * P.keytrack2 * double(kSemitone));
            ratio2_ = float((f + P.coarse2) * kt * 4194304.0);
        }
        auto scale = [&](float velSens, float bp, float lo, float hi) {
            const double vn = (vel + 1.0) * 0.5;
            const double vs = (vn * vn - 0.5) * velSens + 0.5;
            const double d = double(N) - bp;
            double y = (d > 0.0 ? hi : lo) * d;
            if (y > 24.0) y = 24.0;
            return x87exp(y * double(0.0577622f)) * vs;
        };
        {
            const double s = scale(P.velSens1, P.bp1, P.lo1, P.hi1);
            thru_ = float(s * P.thru);
            d1_ = float(s * P.depth1);
        }
        {
            const double D = scale(P.velSens2, P.bp2, P.lo2, P.hi2) * P.depth2;
            d2_ = float(D);
            if (P.envLevel > 1.f) {
                d2Target_ = d2_;
                d2_ = float((2.0 - P.envLevel) * d2_);
            } else {
                d2Target_ = float((double(P.envLevel) * P.envLevel + double(1e-8f)) * D);
            }
        }
        {
            const double c = x87exp((0.5 - P.velToDecay) * vel * 8.0) * P.envRate;
            d2Coef_ = c > 1.0 ? 1.f : float(c);
        }
        if (P.lfCarrier > 0.f)
            fixedInc_ = float((double(rng.bi()) * e_->randStart + 1.0) * P.invSr * P.lfCarrier);
        else
            fixedInc_ = -1.f;
        index_ = 1.f;
        inc_ = 0;
        inc1_ = inc2_ = 0;
    }

    // FUN_1004ab60's increment (exp(rateLn + pitch), at most maxInc = 1), then the FM block part of FUN_1004c920.
    void oscControl() override {
        Voice::oscControl();
        if (wave_ > 1.f) wave_ = 1.f;
        index_ = float(x87exp((double(wave_) - 1.0) * 3.0));
        inc1_ = uint32_t(int(double(ratio1_) * inc_));
        inc2_ = uint32_t(int(double(ratio2_) * inc_));
        d2_ = float((double(d2Target_) - d2_) * d2Coef_ + d2_);
        if (fixedInc_ > 0.f) inc_ = fixedInc_;
    }

    static double saw(uint32_t ph) {
        const uint32_t bits = ph + 0x40000000u;
        float f;
        std::memcpy(&f, &bits, 4);
        return double(f) - 3.0;
    }

    void renderOsc(float* out, int frames) override {
        Svf& s = f_[0];
        for (int n = 0; n < frames && active_; ++n) {
            if (--blockCount_ < 0) controlUpdate();
            ph1_ = (ph1_ + inc1_) & 0x7fffffu;
            double s1 = saw(ph1_);
            s1 = s1 - s1 * s1 * s1;
            ph2_ = (ph2_ + inc2_) & 0x7fffffu;
            double s2 = saw(ph2_);
            s2 = s2 - s2 * s2 * s2;
            double x = (s2 * d2_ + s1 * d1_) * index_ + phase_ + inc_;
            while (x > 1.0) x -= 2.0;
            while (x < -1.0) x += 2.0;
            phase_ = float(x);
            const double o = ((x * x * 0.5 - 1.5) * x * x + 1.0) * x + s1 * thru_;
            // shared filter (as in FUN_1004b2a0), with the x87 keeping hp / bp / t unrounded in the register
            const double hp = ((double(kk_) * s.bp - o) - s.xprev) + s.lp;
            s.hp = float(hp);
            const double bp = s.bp - hp * a_;
            s.bp = float(bp);
            s.lp = float(bp * b_ + s.lp);
            s.xprev = float(o);
            const double t = s.t - ((double(s.t) - s.lp) + s.u) * cc_;
            s.t = float(t);
            s.u = float(t * cc_ + s.u);
            double v;
            switch (tap_) {
                case 0: v = s.lp; break;
                case 1: v = s.bp; break;
                case 2: v = s.hp; break;
                case 3: v = s.u; break;
                case 4: v = s.t; break;
                default: v = 0.0; break;
            }
            if (invert_) v = (double(s.xprev) + s.xprev) - v;
            const double gl = double(gainL_) + incL_;
            gainL_ = float(gl);
            out[2 * n] = float(gl * v + out[2 * n]);
            const double gr = double(gainR_) + incR_;
            gainR_ = float(gr);
            out[2 * n + 1] = float(gr * v + out[2 * n + 1]);
        }
    }

    bool oscFinished() const override { return false; }  // the one-shot test is for sample modes only

    uint32_t ph1_ = 0, ph2_ = 0, inc1_ = 0, inc2_ = 0;  // v[0x32], v[0x37], v[0x33], v[0x38]
    float ratio1_ = 0, ratio2_ = 0;                      // v[0x34], v[0x39]
    float d1_ = 0, thru_ = 0, d2_ = 0, d2Coef_ = 1, d2Target_ = 0;  // v[0x35], v[0x36], v[0x3a], v[0x3b], v[0x3c]
    float fixedInc_ = -1;                                // v[0x3d]
    float index_ = 1;                                    // v[0x12] after the block's exp
};

}  // namespace

bool FMSupported() { return true; }

std::shared_ptr<const TypeParams> computeFMParams(const ParamArray& p, const ParamArray& raw, const ParamArray&,
                                                  const EngineConsts& c, ElementParams&) {
    auto P = std::make_shared<FMParams>();
    P->coarse1 = coarseRatio(p[0x11]);
    P->fine1 = float(double(p[0x12]) * p[0x12] * 0.5);
    P->thru = float(double(p[0x13]) * 3.0);
    P->depth1 = float(double(p[0x14]) * p[0x14] * 3.0);
    P->velSens1 = p[0x15];
    P->lo1 = float(3.0 - double(p[0x16]) * 4.0);
    P->bp1 = float(double(p[0x17]) * double(127.9f));
    P->hi1 = float((2.0 - p[0x18]) * p[0x18] * 4.0 - 2.0);
    P->coarse2 = coarseRatio(p[0x19]);
    P->fine2 = float(double(p[0x1a]) * p[0x1a] * 0.5);
    P->keytrack2 = float(1.0 - p[0x1b]);
    P->depth2 = float(double(p[0x1c]) * p[0x1c] * 3.0);
    P->velSens2 = p[0x1d];
    P->lo2 = float(3.0 - double(p[0x1e]) * 4.0);
    P->bp2 = float(double(p[0x1f]) * double(127.9f));
    P->hi2 = float((2.0 - p[0x20]) * p[0x20] * 4.0 - 2.0);
    P->envRate = float(timeCoefD(float(timeCurveD(p[0x21])), c.blockT));
    P->envLevel = float(double(p[0x22]) + p[0x22]);
    {
        const double v = p[0x23];
        P->lfCarrier = p[0x23] > 0.05f ? float(v * v * v * v * 100.0) : -1.f;
    }
    P->velToDecay = raw[0x24];
    P->invSr = float(1.0 / double(c.sr));
    return P;
}

std::unique_ptr<Voice> makeFMVoice() { return std::make_unique<FMVoice>(); }

}  // namespace hs2
