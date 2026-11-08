#include "hs2/voice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hs2 {

// All arithmetic below mirrors the x87 code at 53-bit precision: `double` = a register value, a `float` assignment =
// a store in the original (fst keeps the register unrounded, so later uses of the same value stay double).

namespace {
enum Tap { kLp, kBp, kHp, kU, kT, kZero };
constexpr double kPiF = double(3.1415927f);
constexpr double kTwoPiD = 6.2831854820251465;  // the qword constant fmod uses (FUN_10049090 cases 17/18)
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// LFO (FUN_10049a60 init / FUN_10048e10 tick)
void Lfo::driftSlope(MsvcRand& rng) {  // wave 9: bounded random walk, step 0.2 (the bounded value is stored first)
    const double r = double(rng.bi()) * double(0.2f) + double(value);
    float s = float(r);
    if (r > 1.0) s = float(2.0 - r);
    else if (r < -1.0) s = float(-2.0 - r);
    slope = float(double(s) - double(value));
}

float Lfo::init(float p01, int dw, MsvcRand& rng) {
    wave = dw == 0 ? 1 : dw == 1 ? 2 : dw == 2 ? 0 : dw;
    phase = p01 + p01;
    if (wave == 3) phase = float(std::fmod(double(phase) + 0.5, 2.0));  // FIX: 10049aa9: fmod(2p + 0.5, 2.0)
    half = 0;
    if (!(phase < 1.f)) {
        half = 1;
        phase = phase - 1.f;
    }
    slope = 0;
    const double p = phase;
    switch (wave) {  // waves 6 and 7 (low half) keep the value word as it is: the note-on seeds it with a rand
        case 0: {
            const double s = std::sin(p * kPiF);
            value = float(half ? -s : s);
            break;
        }
        case 1: value = 0; break;
        case 2: value = 1; break;
        case 3:
            slope = half ? -2.f : 2.f;
            value = float(half ? (1.0 - p) - p : p + p - 1.0);
            break;
        case 4: slope = -1; value = half ? -phase : float(1.0 - p); break;
        case 5: value = half ? -1.f : 1.f; break;
        case 7: if (half) value = -1.f; break;
        case 8: slope = float(double(rng.bi()) - double(value)); break;
        case 9: driftSlope(rng); break;
        default: break;
    }
    return value;
}

float Lfo::tick(float rate, MsvcRand& rng) {
    phase = float(double(rate) + double(phase));
    if (wave == 0) {
        if (!(phase < 1.f)) {
            half = 1 - half;
            phase = float(double(phase) - 1.0);
        }
        const double s = std::sin(double(phase) * kPiF);
        return float(half ? -s : s);
    }
    const double v = double(rate) * double(slope) + double(value);
    value = float(v);
    if (phase < 1.f) return value;
    slope = 0;  // segment end: plan the next one
    phase = float(double(phase) - 1.0);
    half = 1 - half;
    const double p = phase;
    switch (wave) {
        case 1: value = 0; break;
        case 2: value = 1; break;
        case 3:
            slope = half ? -2.f : 2.f;
            value = float(half ? (1.0 - p) - p : p + p - 1.0);
            break;
        case 4: slope = -1; value = half ? -phase : float(1.0 - p); break;
        case 5: value = half ? -1.f : 1.f; break;
        case 6: {
            const double r = double(rng.uni()) + double(value) + 0.5;
            value = float(r > 1.0 ? r - 2.0 : r);
            break;
        }
        case 7: value = half ? -1.f : rng.uni(); break;
        case 8: slope = float(double(rng.bi()) - double(value)); break;
        case 9: driftSlope(rng); break;
        default: break;
    }
    return value;
}

// ---------------------------------------------------------------------------------------------------------------
// mod matrix sources / destinations (FUN_10010930 / FUN_10010ba0)
const float* Voice::sourcePtr(int i) {
    switch (i) {
        case 1: return &one_;
        case 2: return &rand_;
        case 3: return &vel_;
        case 4: return &notePitch_;
        case 5: return &lastPitch_;
        case 6: return &in_->modWheel;
        case 7: return &in_->aftertouch;
        case 8: return &in_->pitchBend;
        case 9: return &in_->breath;
        case 10: return &in_->wheel3;
        case 11: return &in_->foot;
        case 12: return &sus_;   // voice+0x304: sustain pedal (note-on / legato: part+0xd98, CC64: the CC value)
        case 13: return &soft_;  // voice+0x308: soft pedal (part+0xdbc at note-on / legato)
        case 14: case 15: case 16: case 17: case 18: case 19: return &in_->knob[i - 14];
        case 20: return &scale2_;  // FIX: voice+0x310 (FUN_10010870 at note-on)
        case 21: return &in_->seq1;
        case 22: return &in_->seq2;
        case 23: return &lfo1Val_;
        case 24: return &lfo2Val_;
        case 25: return &lfo3Val_;
        case 26: return &env1Level_;
        case 27: return &fenv_.level;
        case 28: return &aenv_.level;
        case 99: return &f_[0].xprev;  // S+H with no source: the audio signal
        default:
            if (i >= 29 && i < 29 + 15) return &mods_[i - 29].last;
            return &zero_;
    }
}

float* Voice::destPtr(int i) {
    switch (i) {
        case 1: return &pitch_;
        case 2: return &wave_;
        case 3: return &cutoff_;
        case 4: return &k_;
        case 5: return &amp_;
        case 6: return &pan_;
        case 7: return &lfo3RateMod_;
        case 8: return &env1RateMod_;
        case 9: return &osc1Pitch_;
        default: return &dummy_;
    }
}

// Matrix pass (FUN_1004ab60 @1004ada0, FUN_10049dd0, FUN_10049e40): normal rows add (mod * w2 + w1) * src.
void Voice::runMatrix() {
    for (int i = 0; i < modCount_; ++i) {
        ModRt& m = mods_[i];
        if (m.special == 0) {
            const double l = (double(*m.mod) * double(m.w2) + double(m.w1)) * double(*m.src);
            m.last = float(l);
            *m.dest = float(l + double(*m.dest));
        } else {
            processSlot(m);
        }
    }
}

// FUN_10049090: processed modifiers. w1 = parameter, w2 = depth, state = per-row state. Most cases store the row
// output (`last`) and then add last * depth to the destination.
void Voice::processSlot(ModRt& m) {
    const double x = *m.src;
    const double w1 = m.w1;
    float& last = m.last;
    float& st = m.state;
    auto keep = [&]() { *m.dest = float(double(last) * double(m.w2) + double(*m.dest)); };
    auto emit = [&](double v) {
        last = float(v);
        keep();
    };
    switch (m.special) {
        case 1: emit(x + w1); return;                                         // Add
        case 2: emit(x * w1); return;                                         // Multiply
        case 3: {                                                             // Drive
            const double v = x * w1;
            emit(v > 1.0 ? 1.0 : v < -1.0 ? -1.0 : v);
            return;
        }
        case 4: emit(x > w1 ? w1 : x); return;                                // Clip+
        case 5: emit(x < w1 ? w1 : x); return;                                // Clip-
        case 6: emit(x > w1 ? (w1 + w1) - x : x); return;                     // Rect+
        case 7: emit(x < w1 ? (w1 + w1) - x : x); return;                     // Rect-
        case 8: emit(((x * w1 * x - w1) + 1.0) * x); return;                  // Curve
        case 9: emit(std::sin(x * w1)); return;                               // Sine
        case 10: emit(expF(float(x * w1))); return;                           // Exp
        case 11: {                                                            // Log
            const double s = x + w1;
            emit(s > double(4.54e-5f) ? std::log(s) : -10.0);
            return;
        }
        case 12: emit(x > w1 ? 1.0 : 0.0); return;                            // Switch
        case 13: emit(std::floor(double(float(x * w1 + 0.5))) * double(st)); return;  // Quant (odd steps)
        case 14: emit((std::floor(double(float(x * w1))) + 0.5) * double(st)); return;  // Quant (even steps)
        case 15: {                                                            // S+H
            const double s = double(st) - w1;
            st = float(s);
            if (s < 0.0) {
                st = float(s + 1.0);
                emit(x);
            } else {
                keep();
            }
            return;
        }
        case 16: {                                                            // Drift (random walk, scaled by src)
            if (last < st) {
                const double v = ((double(st) - double(last)) + 1.0) * w1 + double(last);
                last = float(v);
                if (v > double(st)) st = rng_->bi();
            } else {
                const double v = (double(st) - double(last)) * w1 - w1 + double(last);
                last = float(v);
                if (v < double(st)) st = rng_->bi();
            }
            *m.dest = float(x * double(m.w2) * double(last) + double(*m.dest));
            return;
        }
        case 17:                                                              // Sine Amp
            last = float(std::sin(double(st)) * x);
            st = float(std::fmod(w1 + double(st), kTwoPiD));
            keep();
            return;
        case 18:                                                              // Sine Freq
            last = float(std::sin(double(st)));
            st = float(std::fmod(expF(float(x * double(2.3025851f))) * w1 + double(st), kTwoPiD));
            keep();
            return;
        case 19:                                                              // Lin Ramp
            if (st < 1.f) {
                last = float(x * double(st));
                st = float(w1 + double(st));
                keep();
            } else {
                emit(x);
            }
            return;
        case 20:                                                              // Delay Ramp
            if (st < 1.f) {
                if (st > 0.5f) last = float((double(st) + double(st) - 1.0) * x);
                st = float(w1 + double(st));
                keep();
            } else {
                emit(x);
            }
            return;
        case 21: {                                                            // Exp Ramp
            const double s = (1.0 - double(st)) * w1 + double(st);
            st = float(s);
            emit(s * x);
            return;
        }
        case 22:                                                              // Log Ramp
            if (st < 1.f) {
                last = float(x * double(st));
                st = float(w1 * double(st));
                keep();
            } else {
                emit(x);
            }
            return;
        case 23:                                                              // Tri Env
            if (st < 1.f) {
                const double f = st < 0.5f ? double(st) : 1.0 - double(st);
                last = float(f * x + f * x);
                st = float(w1 + double(st));
            }
            keep();
            return;
        case 24:                                                              // Gate Env
            if (st >= 1.f) {
                emit(0.0);
                return;
            }
            last = float(x);
            st = float(w1 + double(st));
            keep();
            return;
        case 25: emit((x - double(last)) * w1 + double(last)); return;         // Log Lag
        case 26: {                                                            // Lin Lag
            if (x > double(last)) {
                const double v = w1 + double(last);
                last = float(v);
                if (v > x) last = float(x);
            } else if (x < double(last)) {
                const double v = double(last) - w1;
                last = float(v);
                if (v < x) last = float(x);
            }
            keep();
            return;
        }
        case 27:                                                              // Peak Hold
            if (x > double(last)) emit(x);
            else emit((double(1e-10f) - double(last)) * w1 + double(last));
            return;
        case 28:                                                              // Diff Hold
            last = float((((double(1e-10f) - double(last)) * w1 + x) - double(st)) + double(last));
            st = float(x);
            keep();
            return;
        case 29: emit((x - w1) * (x - w1) * -8.0); return;                   // X-Fade
        case 0x61: { const double v = x - w1; emit(v > 0.0 ? 0.0 : v); return; }
        case 0x62: { const double v = x - w1; emit(v < 0.0 ? 0.0 : v); return; }
        case 99: {                                                            // Amp (multiplicative)
            const double l = (double(*m.mod) * double(m.w2) + w1) * x;
            last = float(l);
            *m.dest = float((l + 1.0) * double(*m.dest));
            return;
        }
        case 100: {                                                           // Amp, bipolar controller
            const double l = (x + x) * w1 - w1;
            last = float(l);
            *m.dest = float((l + 1.0) * double(*m.dest));
            return;
        }
        case 0x77:
            if (st >= 1.f) {
                emit(0.0);
                return;
            }
            last = float(x - x * double(st));
            st = float(w1 + double(st));
            keep();
            return;
        case 0x78:
            if (st >= 1.f) {
                emit(0.0);
                return;
            }
            last = st > 0.5f ? float((2.0 - (double(st) + double(st))) * x) : float(x);
            st = float(w1 + double(st));
            keep();
            return;
        case 0x79: {
            const double s = (1.0 - double(st)) * w1 + double(st);
            st = float(s);
            emit(x - s * x);
            return;
        }
        case 0x7a:
            if (st < 1.f) {
                last = float(x - x * double(st));
                st = float(w1 * double(st));
            }
            keep();
            return;
        case 0x7b:
            if (st < 1.f) {
                const double f = st < 0.5f ? 0.5 - double(st) : double(st) - 0.5;
                last = float(f * x + f * x);
                st = float(w1 + double(st));
                keep();
            } else {
                emit(x);
            }
            return;
        case 0x7c:
            if (st < 1.f) {
                last = 0;
                st = float(w1 + double(st));
                keep();
            } else {
                emit(x);
            }
            return;
        default: keep(); return;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Note-on (FUN_10014910), in the original's order (the rand draws must stay in sequence).
void Voice::start(const ElementParams& e, const EngineConsts& c, const VoiceStart& vs, const PatchInputs& in,
                  MsvcRand& rng) {
    e_ = &e;
    in_ = &in;
    rng_ = &rng;
    c_ = c;
    s_ = vs.sample;
    z_ = vs.zone;
    active_ = true;
    released_ = false;
    note_ = vs.note;
    relKey_ = vs.note;
    bpm_ = vs.bpm;
    ch_ = s_ ? s_->channels : 1;
    step_ = ch_;
    // FIX: Force Mono: a stereo sample plays in a mono render mode (0x11 / 0x12, compressed 2 / 0x10) that reads the
    // left channel only, with the stereo stride (FUN_10014910 @100164a0 / @10016606)
    if (ch_ == 2 && e.forceMono) ch_ = 1;
    maxInc_ = 1.f;      // v[0x86] (@10014988); the type cases may change both
    rateLn_ = c.lnRef;  // v[0x87] = elem+0x14

    // LFO3 (@10014a6c): FIX: the first rand01 is stored in the LFO state's value word (+0x2d0), which the S+H and
    // drift waves keep or start from; then a random phase if Phase > 0.9 or Sync is off.
    lfo3_.value = rng.uni();
    const float ph = (e.lfo3.phase > 0.9f || e.lfo3.sync == 0) ? rng.uni() : e.lfo3.phase * 1.1111f;
    lfo3Val_ = lfo3_.init(ph, e.lfo3.wave, rng);
    lfo3Rate_ = e.lfo3.sync >= 3 ? float(double(vs.bpm) * double(e.lfo3.tempoRate)) : e.lfo3.rate;
    rand_ = rng.bi();
    // FIX: the note-on does not set the LFO1/2 words (v[0xae], v[0xaf]): the note-on matrix passes see the slot's
    // previous values until the first control update reads the part's LFO array.
    noteGain_ = vs.noteGain;
    unison = vs.unisonIndex;
    sus_ = vs.sustainWord;  // @10015768: v[0xc1] = (float)part+0xd98, v[0xc2] = part+0xdbc
    soft_ = vs.softWord;
    pitchSrc_ = e.pitchBendOn ? &in.pitchOfs : &in.fineTune;  // v[0x93] (@10015807)

    const int N = vs.note + vs.transpose;
    // v[0xbe] = ((int)(cents + (N - 60) * 100) + tuning) * ln2/1200 (tuning: FUN_10010870 of the untransposed note)
    scale2_ = vs.scale2;
    notePitch_ = float((double((N - 60) * 100 + vs.cents) + vs.tune) * double(kCent));
    // FIX: velocity rounded once: float((vel - 1) * 0.015873 - 1)
    vel_ = e.receiveVelocity ? float(double(vs.velocity - 1) * double(0.015873f) - 1.0) : 0.f;
    const double vel = vel_;
    delay_ = 0;
    if (e.noteDelayMode == 1) {  // @10014c43: (1 + randpm * Random) * t(Delay Time) * (1 - VelToDelay * vel) * sr
        const float t = float(timeCurveD(e.fadeTime));
        const double t1 = (double(rng.bi()) * double(e.randDelay) + 1.0) * double(t);
        delay_ = std::max(0, int((t1 - double(e.velToDelay) * vel * t1) * double(c.sr)));
    } else if (e.noteDelayMode == 2) {  // @10014bf3: tempo-synced: trunc(1 / (noteValue(1 - Delay Time) * beats/sample))
        delay_ = int(int64_t(1.0 / (double(noteValue(float(1.0 - double(e.fadeTime)))) * vs.bps)));
    }

    // unison stack position (param_7): p5 = (k - 1) / N - 1/2
    const int uk = vs.unisonIndex;
    const float p5 = uk ? float(double(uk - 1) * double(e.unisonInv) - 0.5) : 0.f;
    {  // glide start (@10014cdd) and legato start (@10014d42) from the element's previous target (+0x4f8)
        const float r1 = rng.uni(), r2 = rng.uni();
        double t = ((double(r1) - double(r2)) * double(e.randomPitch) +
                    double(e.unisonVoices) * double(e.unisonDetune) * double(p5)) +
                   double(e.keytrack) * double(notePitch_);
        // @10015824: a detune of at least a semitone (arp transposition) is added again, unscaled by the keytrack,
        // when the element has Patch Pitch Bend on
        if (std::abs(vs.cents) > 99 && e.pitchBendOn) t = double(vs.cents) * double(kCent) + t;
        const float target = float(t);
        glideTarget_ = glide_ = target;
        const int m = e.glideMode;
        bool fromLast;
        if (!e.receiveCC || e.patchGlide == 1) fromLast = (m == 2 && vs.otherHeld) || m == 3;
        else if (e.patchGlide == 0) fromLast = false;
        else if (e.patchGlide == 2) fromLast = vs.otherHeld;
        else fromLast = true;
        lastGlide_ = vs.lastGlide;
        if (fromLast && lastGlide_) glide_ = *lastGlide_;
        if (lastGlide_) *lastGlide_ = target;
        if (m != 0)  // Attack Bend
            glide_ = float((double(e.velToBend) * vel + (1.0 - std::fabs(double(e.velToBend)))) *
                               double(e.attackBend) + double(glide_));
    }
    notePitch_ = float(double(vs.transpose) * double(kSemitone) + double(notePitch_));  // FIX: transpose twice

    // v[0x95] wave offset
    waveMod_ = float((double(rng.bi()) * double(e.randWave) + double(e.velToWave) * vel) +
                     double(e.keyToWave) * double(notePitch_));
    // filter state and mode (taps)
    f_[0] = f_[1] = Svf{};
    resoLimit_ = 0.77f;
    c2Scale_ = 0.63f;
    freqFlag_ = true;
    invert_ = false;
    energy_ = 0;
    switch (e.filterMode) {
        case 1: tap_ = kU; break;
        case 2: tap_ = kLp; break;
        case 3: tap_ = kT; break;
        case 4: tap_ = kBp; break;
        case 5: tap_ = kHp; resoLimit_ = 0.3f; break;
        case 6: tap_ = kBp; invert_ = true; resoLimit_ = 0.5f; break;
        case 7: tap_ = kHp; invert_ = true; resoLimit_ = 0.5f; break;
        case 8: tap_ = kLp; freqFlag_ = false; break;
        default: tap_ = kZero; invert_ = true; break;
    }
    // v[0x97] cutoff offset, v[0x96] resonance offset (a sum > 0.98 pre-charges the filter)
    cutoffMod_ = float((double(rng.bi()) * double(e.randCutoff) + (vel - 1.0) * double(e.velToCutoff)) *
                       double(kTenOctaves));
    {
        const double vr = double(e.velToReso) * vel;
        resoMod_ = float(vr);
        if (vr + double(e.reso) > double(0.98f)) f_[0].t = f_[0].bp = 2.f;
    }
    // v[0x99] pan offset. FIX: the alternate pan uses the element's note counter.
    panMod_ = float(((double(rng.bi()) * double(e.randPan) + (double(int(vs.altCounter & 1u)) - 0.5) * double(e.altPan)) +
                     double(p5) * double(e.unisonPan)) +
                    double(e.panKey) * double(notePitch_));
    {  // amp env peak (@1001507b): velocity curve (raw velocity) times the key/velocity crossfade gain
        double f = ((double(vs.velocity - 1) * double(0.015873f) - 1.0) + 1.0) * 0.5;
        const float am = e.aenv.velToLevel;
        if (am < 0.f) f = 1.0 - f;
        const double a = std::fabs(am);
        if (a > 1.0) f = (((f * f - f) * (a - 1.0) + f) + double(0.07f)) - a * double(0.02f);
        else f = (f - 0.5) * a + double(0.55f);
        aenv_.peak = float(((double(0.07f) - a * double(0.02f)) + f) * double(vs.noteGain));
    }
    // v[0x98] level factor (mono unison stack: (N - 7)^2 * 0.0067 + 0.6)
    levelMod_ = (e.unisonVoices && uk) ? float(double((e.unisonVoices - 7) * (e.unisonVoices - 7)) * double(0.0067f) +
                                               double(0.6f))
                                       : 1.f;
    env1Level_ = e.env1Start;
    env1Stage(0);
    {  // filter env peak (@10015171)
        double g = (vel + 1.0) * 0.5;
        const float fm = e.fenv.velToLevel;
        if (fm < 0.f) g = 1.0 - g;
        const double b = std::fabs(fm);
        if (b > 1.0) g = (g * g - g) * (b - 1.0) + g;
        else g = (g - 0.5) * b + 0.5;
        fenv_.peak = float(g + g);
    }
    fenv_.level = 0;
    nextStage(fenv_, false, 0);
    aenv_.level = 0;
    nextStage(aenv_, true, 0);
    gainL_ = gainR_ = 0;

    // mod matrix runtime rows
    modVersion_ = e.modVersion;
    modCount_ = e.modCount;
    for (int i = 0; i < modCount_; ++i) {
        const ModSlot& ms = e.mods[i];
        ModRt& m = mods_[i];
        m.src = sourcePtr(ms.src);
        m.mod = sourcePtr(ms.mod);
        m.w1 = ms.w1;
        m.w2 = ms.w2;
        m.last = 0;
        m.dest = destPtr(ms.dest);
        m.state = ms.w6;
        m.special = ms.special;
    }

    // The type's case of the note-on switch. FIX: only the Sample case (@10016298) runs the note-on matrix pass
    // FUN_10049dd0 (wave = elem+0x5c0 + v[0x95]; the other destinations accumulate onto their current words).
    float startMod = 0;
    if (e.type == ElementType::Sample) {
        wave_ = float(double(e.wave5c0) + double(waveMod_));
        runMatrix();
        startMod = wave_;
    }
    startOsc(vs, startMod, rng);
    if (!active_) return;

    {  // pan-law level compensation (@10016684): v[0x98] *= 1 - 0.5858 * min(|v[0x99] + pan - 0.5|, 0.5)
        double pd = std::fabs((double(panMod_) + double(e.pan)) - 0.5);
        if (pd > 0.5) pd = 0.5;
        levelMod_ = float((1.0 - pd * double(0.5858f)) * double(levelMod_));
    }
    // FIX: Note-on Click (@100166c5), every type: with Attack < 0.0005 and Click > 0.01, a matrix pass with amp = 1
    // (FUN_10049e40), the amp env starts in its decay stage and the gain ramps start at the click level.
    if (e.aenv.attack < 0.0005f && e.click > 0.01f) {
        amp_ = 1.f;
        runMatrix();
        const float ampPass = amp_;
        nextStage(aenv_, true, 1);
        const double X = (double(0.0005f) - double(e.aenv.attack)) * double(e.level) * double(aenv_.peak) *
                         double(ampPass) * double(e.click) * 8000.0;
        const double R = (double(panMod_) + double(e.pan)) * X;
        gainR_ = float(R);
        gainL_ = float(X - R);
        onClick(X);
    }
    refresh();
    blockCount_ = 0;
    {  // glide speed (end of FUN_10014910): min(exp(VelToGlide * vel) * coef, 1)
        const double g = x87exp(double(e.velToGlide) * vel) * double(e.glideCoef);
        glideCoef_ = g > 1.0 ? 1.f : float(g);
    }
    waitingForNoteOff_ = e.noteDelayMode == 3;
    heldSamples_ = 0;
    // The note-on leaves the LFO3 / Env1 rate words (v[0xb2], v[0x9e]) as the slot's previous note left them (0 in a
    // fresh voice), so the first control block runs with those (FIX).
}

// FUN_10049eb0: the voice's copies of the element values. Called at the end of the note-on and again by the render
// functions when the element's version (+0x5ec) differs from the voice's (+0x520); that second call drops the
// note-on's Vel->Glide factor (FIX).
void Voice::refresh() {
    pitchBase_ = e_->pitchBase;
    waveBase_ = float(double(e_->wave5c0) + double(waveMod_));
    resoBase_ = float(double(e_->reso) + double(resoMod_));
    cutoffBase_ = float(double(e_->cutoffLn) + double(cutoffMod_));
    levelBase_ = float(double(e_->level) * double(levelMod_));
    panBase_ = float(double(e_->pan) + double(panMod_));
    glideCoef_ = e_->glideCoef;
    lfo3Rate_ = e_->lfo3.sync >= 3 ? float(double(bpm_) * double(e_->lfo3.tempoRate)) : e_->lfo3.rate;
}

void Voice::inheritSlot(const Voice& o) {
    lfo1Val_ = o.lfo1Val_;
    lfo2Val_ = o.lfo2Val_;
    stamp = o.stamp;
    lfo3RateMod_ = o.lfo3RateMod_;
    env1RateMod_ = o.env1RateMod_;
    amp_ = o.amp_;
    lastPitch_ = o.lastPitch_;
    paramVersion_ = o.paramVersion_;
    y0_[0] = o.y0_[0];
    y0_[1] = o.y0_[1];
    slope_[0] = o.slope_[0];
    slope_[1] = o.slope_[1];
}

// Note-off (FUN_10017880): env 1 -> release, filter env -> release; the amp env is released only if the note is
// below the "Undamped >" key, or Fade > 0, or Sustain > 0 with no fade. Otherwise (one-shots) the decay runs on.
void Voice::release() {
    if (!active_ || released_) return;
    released_ = true;
    if (delay_ > 0) {  // FIX: note-off while the note delay is pending (v[1] > 0): silenced and marked stolen
        aenv_.level = aenv_.coef = aenv_.target = 0.f;
        aenv_.hi = 999.f;
        aenv_.lo = -999.f;
        mayEnd_ = 2;
        delay_ = 0;
        return;
    }
    if (waitingForNoteOff_) {  // release trigger: start now, quieter the longer the key was held
        waitingForNoteOff_ = false;
        if (e_->fadeTime > 0.01f) {
            // FIX: @10018990, x87: v[1] (= -1 - held frames) * (float)(1/sr) * 3.45 stored as float, / t(Fade Time)
            const float x = float(double(-1 - heldSamples_) * double(float(1.0 / double(c_.sr))) * double(3.45f));
            const double g = x87exp(double(x) / timeCurveD(e_->fadeTime));
            aenv_.peak = float(g * double(aenv_.peak));
            aenv_.hi = float(g * double(aenv_.hi));
            aenv_.target = float(g * double(aenv_.target));
        }
        return;
    }
    // Analog glide freezes where it is (and becomes the element's last pitch); Release Bend moves the target
    if (e_->glideAnalog) {
        glideTarget_ = glide_;
        if (lastGlide_) *lastGlide_ = glide_;
    }
    if (e_->glideMode > 0) glideTarget_ += e_->releaseBend;
    env1Stage(4);
    nextStage(fenv_, false, 3);
    const auto& a = e_->aenv;
    // the key test uses the note-off's note byte (-1 for the sustain pedal's release, FUN_10017880 @10018879)
    if (relKey_ < e_->undampedKey || a.fade > 0.f || (a.sustain > 1e-8f && a.fade == 0.f)) nextStage(aenv_, true, 3);
}

void Voice::syncModRow() {
    if (!e_ || modVersion_ == e_->modVersion) return;
    modVersion_ = e_->modVersion;
    const int i = int(modVersion_ & 0xff);
    if (i >= 16) return;
    const ModSlot& ms = e_->mods[i];
    ModRt& m = mods_[i];
    m.src = sourcePtr(ms.src);
    m.mod = sourcePtr(ms.mod);
    m.special = ms.special;
    m.w1 = ms.w1;
    m.w2 = ms.w2;
    m.state = ms.w6;
    m.last = 0;
    m.dest = destPtr(ms.dest);
}

void Voice::steal() {  // FUN_100101a0: FUN_1004a340(4, 4)
    nextStage(aenv_, true, 4);
}

// FUN_10016820: retrigger with a new note in place (mono legato, Envelope Mode Analog). The list entry's note bytes are
// updated by the caller.
void Voice::legato(int note, int transpose, int velocity, int mode, int patchGlide, MsvcRand& rng, int cents,
                   float sustainWord, float softWord, double tune) {
    const ElementParams& e = *e_;
    note_ = note;
    relKey_ = note;
    released_ = false;
    // v[0xbe] = (tuning + (cents + (note + transpose - 60) * 100)) * cent: the transpose only once here
    notePitch_ = float((tune + double((note + transpose - 60) * 100 + cents)) * double(kCent));
    const float p5 = unison ? float(double(unison - 1) * double(e.unisonInv) - 0.5) : 0.f;
    const float r1 = rng.uni(), r2 = rng.uni();
    double target = (double(e.keytrack) * double(notePitch_) +
                     double(e.unisonVoices) * double(e.unisonDetune) * double(p5)) +
                    (double(r1) - double(r2)) * double(e.randomPitch);
    if (std::abs(cents) > 99 && e.pitchBendOn) target = double(cents) * double(kCent) + target;  // @10016774
    const int m = e.glideMode;
    bool jump;  // set v[0x91] (no glide)
    if (!e.receiveCC || patchGlide == 1) jump = !(m == 3 || (m == 2 && mode != 0));
    else if (patchGlide == 0) jump = true;
    else if (patchGlide == 3) jump = false;
    else jump = mode == 0;  // patch Glide 2 "Legato"
    if (jump) glide_ = float(target);
    glideTarget_ = float(target);
    if (lastGlide_) *lastGlide_ = float(target);
    const double vel = e.receiveVelocity ? double(velocity - 1) * double(0.015873f) - 1.0 : 0.0;
    if (mode == 0) {
        vel_ = float(vel);
        sus_ = sustainWord;  // @10016808: +0x304 = (float)part+0xd98, +0x308 = part+0xdbc
        soft_ = softWord;
        {  // amp env peak: velocity curve of the raw velocity times the note-on gain word (+0x530)
            double f = ((double(velocity - 1) * double(0.015873f) - 1.0) + 1.0) * 0.5;
            const float am = e.aenv.velToLevel;
            if (am < 0.f) f = 1.0 - f;
            const double a = std::fabs(am);
            if (a > 1.0) f = (((f * f - f) * (a - 1.0) + f) + double(0.07f)) - a * double(0.02f);
            else f = (f - 0.5) * a + double(0.55f);
            aenv_.peak = float(((double(0.07f) - a * double(0.02f)) + f) * double(noteGain_));
        }
        {
            double g = (vel + 1.0) * 0.5;
            const float fm = e.fenv.velToLevel;
            if (fm < 0.f) g = 1.0 - g;
            const double b = std::fabs(fm);
            if (b > 1.0) g = (g * g - g) * (b - 1.0) + g;
            else g = (g - 0.5) * b + 0.5;
            fenv_.peak = float(g + g);
        }
    }
    if (m != 0) {
        const double g = x87exp(vel * double(e.velToGlide)) * double(e.glideCoef);
        glideCoef_ = g > 1.0 ? 1.f : float(g);
    }
    if (mode <= 1) {
        if (e.lfo3.sync == 2) {
            lfo3_.value = rng.uni();
            const float ph = (e.lfo3.phase > 0.9f || e.lfo3.sync == 0) ? rng.uni() : e.lfo3.phase * 1.1111f;
            lfo3Val_ = lfo3_.init(ph, e.lfo3.wave, rng);
        }
        env1Level_ = e.env1Start;
        env1Stage(0);
    }
    if (mode <= 2) {
        nextStage(aenv_, true, -1);
        if (mode == 0 && e.type == ElementType::Analog && e.aenv.attack < 0.0005f && e.click > 0.01f) {
            nextStage(aenv_, true, 1);
            legatoClick((double(0.0005f) - double(e.aenv.attack)) * double(e.level) * double(aenv_.peak) *
                        double(e.click) * 8000.0);
        }
    }
    if (mode <= 3) nextStage(fenv_, false, -1);
}

// FUN_1004a340, env 1: linear segments; lengths in control blocks (host tempo factor 1).
void Voice::env1Stage(int stage) {
    env1Stage_ = stage + 1;
    env1Time_ = 0;
    float len, target;
    switch (stage) {
        case 0: len = e_->env1Time[0]; target = e_->env1Level[0]; break;
        case 1: env1Level_ = e_->env1Level[0]; len = e_->env1Time[1]; target = e_->env1Level[1]; break;
        case 2: env1Level_ = e_->env1Level[1]; len = e_->env1Time[2]; target = e_->env1Level[2]; break;
        case 3: env1Level_ = e_->env1Level[2]; env1Len_ = 1e10f; env1Slope_ = 0; return;
        case 4: len = e_->env1Time[3]; target = e_->env1Level[3]; break;
        default: env1Level_ = e_->env1Level[3]; env1Len_ = 1e10f; env1Slope_ = 0; return;
    }
    env1Len_ = len = float(tempoFactor() * double(len));  // FUN_1004a340: fVar7 * time
    env1Slope_ = float((double(target) - double(env1Level_)) / double(len));
}

// FUN_1004a340 for env 3 (filter) and 4 (amp); `from` = the stage being left (0 = note-on).
void Voice::nextStage(ExpEnv& env, bool isAmp, int from) {
    const EnvParams& p = isAmp ? e_->aenv : e_->fenv;
    const float T = c_.blockT;
    const double key = notePitch_, vel = vel_;
    const double tf = tempoFactor();  // (120 / bpm - 1) * Tempo to Env Rates + 1 (FUN_1004a340 fVar7)
    int stage = from + 1;
    env.stage = stage;
    if (stage <= 1) {  // attack (the from == -1 offsets elem+0x870/0x874 belong to mono legato: not modelled)
        env.stage = 1;
        if (isAmp) mayEnd_ = 0;
        env.hi = env.peak;
        env.lo = -256.f;
        env.target = float(double(env.peak) * double(3.4f));
        // from == -1 (mono legato, FUN_10016820): the attack time gets Leg>Attack / Leg>F.Attack (+0x870 / +0x874)
        const double off = from == -1 ? double(isAmp ? e_->legAttack : e_->legFAttack) : 0.0;
        const float t = float((off + double(p.attack)) * expF(float(double(p.velToAttack) * vel)) * tf);
        env.coef = float(timeCoefD(t, float(double(T) * double(0.1f))));
        return;
    }
    if (stage == 2) {  // decay
        env.level = env.hi;
        const double S = double(env.level) * double(p.sustain);
        env.lo = float(S);
        if (isAmp && S < double(1e-6f) && !(p.fade > 0.f)) mayEnd_ = 1;
        env.hi = 256.f;
        env.target = float(double(env.lo) - (double(env.level) - double(env.lo)) * double(p.curve));
        const float t = float(expF(float(double(p.decayKey) * key + double(p.velToDecay) * vel)) * double(p.decay) * tf);
        const double lr = std::log((double(p.curve) + double(0.01f)) / (double(p.curve) + 1.0));
        env.coef = float(timeCoefD(t, float(lr * double(T) * double(-0.3f))));
        if (!(p.sustain > 0.98f)) return;
        stage = env.stage = 3;
    }
    if (stage == 3) {  // sustain / fade
        env.target = env.lo;
        env.level = env.lo;
        env.lo = -256.f;
        env.hi = 256.f;
        if (p.fade != 0.f) {
            if (p.fade > 0.f) {
                env.target = env.peak;
            } else {
                env.target = 0.f;
                if (isAmp) mayEnd_ = 1;
            }
        }
        const float t = float(std::fabs(double(p.fade)) * x87exp(double(p.decayKey) * key * 0.5) * tf);
        env.coef = float(timeCoefD(t, T));
        if (!(isAmp && e_->noteDelayMode == 3)) return;
        env.stage = 4;  // release-triggered elements go straight from sustain to release
    }
    // release (amp: from 4 = voice-steal fast fade, from 5 = the decay time)
    if (isAmp) mayEnd_ = 1;
    env.lo = -256.f;
    env.hi = 256.f;
    env.target = 1e-10f;
    if (isAmp && from == 4) {
        env.coef = float(timeCoefD(e_->muteFadeout, T));
        mayEnd_ = 2;  // stolen (FUN_100101a0)
        return;
    }
    const float base = (isAmp && from == 5) ? p.decay : p.release;
    const float t = float(x87exp(double(p.relKey) * key + double(p.relVel) * vel) * double(base) * tf);
    env.coef = float(timeCoefD(t, T));
}

// FUN_1004ab60: the control update, every 32 samples.
void Voice::controlUpdate() {
    if (in_->lfoArr) {  // FIX: the next (LFO1, LFO2) pair of this part render call (voice+0x2e4)
        lfo1Val_ = in_->lfoArr[2 * lfoRead_];
        lfo2Val_ = in_->lfoArr[2 * lfoRead_ + 1];
        ++lfoRead_;
    } else {
        lfo1Val_ = in_->lfo1;
        lfo2Val_ = in_->lfo2;
    }
    if (lfo3RateMod_ < 0.f) lfo3RateMod_ = 0.f;
    else if (lfo3RateMod_ > 10.f) lfo3RateMod_ = 10.f;
    lfo3Val_ = lfo3_.tick(float(double(lfo3Rate_) * double(lfo3RateMod_)), *rng_);
    {  // env 1. Original quirk: the low clamp of the rate tests the amp word (+0xa8, last block's output amp).
        float r = env1RateMod_;
        if (amp_ < 0.f) r = 0.f;
        else if (r > 10.f) r = 10.f;
        const float t = float(double(r) + double(env1Time_));
        env1Time_ = t;
        env1Level_ = float(double(r) * double(env1Slope_) + double(env1Level_));
        if (t > env1Len_) env1Stage(env1Stage_);
    }
    auto stepEnv = [&](ExpEnv& env, bool isAmp) {  // the bound test uses the unrounded level
        const double l = (double(env.target) - double(env.level)) * double(env.coef) + double(env.level);
        env.level = float(l);
        if (l > double(env.hi) || l < double(env.lo)) nextStage(env, isAmp, env.stage);
    };
    stepEnv(fenv_, false);
    stepEnv(aenv_, true);

    const double g = (double(glideTarget_) - double(glide_)) * double(glideCoef_) + double(glide_);
    glide_ = float(g);
    pitch_ = float((double(*pitchSrc_) + double(pitchBase_)) + g);
    wave_ = waveBase_;
    osc1Pitch_ = osc1PitchBase_;
    k_ = resoBase_;
    amp_ = 1.f;
    pan_ = panBase_;
    lfo3RateMod_ = 1.f;
    env1RateMod_ = 1.f;
    cutoff_ = float(double(e_->fenvAmount) * double(fenv_.level) + double(cutoffBase_));
    runMatrix();
    lastPitch_ = pitch_;

    oscControl();
    filterControl(x87exp(double(lastPitch_) * double(e_->cutoffKey) + double(cutoff_)));

    // gains: the amp word becomes the output level, the pan word its right part
    if (amp_ < 0.f) amp_ = 0.f;
    const double A = double(levelBase_) * double(aenv_.level) * double(amp_);
    amp_ = float(A);
    if (pan_ < 0.f) pan_ = 0.f;
    else if (pan_ > 1.f) pan_ = 1.f;
    const double R = A * double(pan_);
    pan_ = float(R);
    incR_ = float((R - double(gainR_)) * 0.03125);
    incL_ = float(((A - R) - double(gainL_)) * 0.03125);
    blockCount_ = kBlock - 1;
}

// FUN_1004ab60 @1004ae1b: SVF coefficients (b = cutoff, a = damping, kk = resonance feedback) and the one-pole.
void Voice::filterControl(double F) {
    b_ = float(F);
    {
        const double c2 = F * double(c2Scale_);
        cc_ = c2 > 1.0 ? 1.f : float(c2);
    }
    // denormal guards (both channels' bp and t, as the original does for every mode)
    const double bp0 = double(f_[0].bp) + double(1e-8f);
    f_[0].bp = float(bp0);
    f_[0].t = float(double(f_[0].t) + double(1e-8f));
    f_[1].bp = float(double(f_[1].bp) + double(1e-8f));
    f_[1].t = float(double(f_[1].t) + double(1e-8f));
    const double k0 = double(0.95f) - double(k_);
    float k = float(k0);
    if (k0 < double(0.15f)) {  // near self-oscillation: amplitude-dependent damping
        const double m = F > 1.0 ? 0.0 : (F - 1.0) * double(0.05f);
        if (double(k) < m) k = float(m);
        double en = (double(f_[0].lp) * double(f_[0].lp) + bp0 * bp0) * double(0.005f);
        if (ch_ > 1) {  // stereo modes (>= 0x13): the louder channel, from the stored words
            const double en2 =
                (double(f_[1].lp) * double(f_[1].lp) + double(f_[1].bp) * double(f_[1].bp)) * double(0.005f);
            if (en2 > en) en = float(en2);
        }
        const double ed = (en - double(energy_)) * double(0.05f) + double(energy_);
        energy_ = float(ed);
        k = float(ed + double(k));
    }
    if (k > 1.f) k = 1.f;
    const double cap = double(1.92f) - double(k) * double(0.85f);
    a_ = float(cap);
    if (F > cap) {
        double t = F - cap;
        if (t > 1.0) t = 1.0;
        const double k2 = (double(resoLimit_) - double(k)) * t + double(k);
        k = float(k2);
        const float cap2 = float(double(1.92f) - k2 * double(0.85f));
        a_ = cap2;
        if (F > double(cap2)) b_ = cap2;
    } else if (freqFlag_) {
        a_ = float(F);
    }
    kk_ = k;
}

void Voice::endCheck() {
    if (!active_) return;
    if (mayEnd_ && aenv_.level < 1e-4f) active_ = false;  // released / faded
    else if (oscFinished()) active_ = false;              // e.g. a one-shot sample that has ended
}

void Voice::render(float* out, int frames) {
    lfoRead_ = 0;  // render entry: voice+0x2e4 = the part's LFO array
    if (!active_) return;
    if (paramVersion_ != e_->version) renderEntry();
    if (waitingForNoteOff_) {
        heldSamples_ += frames;
        return;
    }
    if (delay_ > 0) {  // FIX: note delay: no control updates while waiting (render functions return early)
        if (frames <= delay_) {
            delay_ -= frames;
            return;
        }
        out += 2 * delay_;
        frames -= delay_;
        delay_ = 0;
    }
    if (paramVersion_ != e_->version) {  // render entry (e.g. 1004b123): FUN_10049eb0 when the version differs
        paramVersion_ = e_->version;
        refresh();
        typeRefresh();
    }
    renderOsc(out, frames);
}

// Sample render loop (render modes 0x10..0x17, e.g. 1004b0e0): linear interpolation, SVF + one-pole, gain ramps.
void Voice::renderOsc(float* out, int frames) {
    const float* data = s_->data.data();
    const bool comp = s_->format == 1 && !s_->delta.empty();
    for (int n = 0; n < frames && active_; ++n) {
        if (--blockCount_ < 0) controlUpdate();
        // FIX: render mode 0x14 (FUN_1004bb10: compressed stereo, filter modes other than 1 / 3) skips the filter and
        // the output while both gains and their ramps are zero; only the sample position advances
        const bool quiet = quietSkip_ && double(gainR_) + double(gainL_) == 0.0 && incL_ == 0.f && incR_ == 0.f;
        while (phase_ > 1.f) {
            phase_ -= 1.f;
            if (comp) {  // FIX: compressed modes (e.g. 1004b0e0): y += delta, then the loop wrap, then the next delta
                for (int c = 0; c < ch_; ++c) y0_[c] += slope_[c];
                if (pos_ == end_) {  // FUN_1004aad0
                    if (loopLen_ == 0) {
                        step_ = 0;  // reads the 0x40 header bytes: delta 0 from now on
                    } else {
                        pos_ -= loopLen_;
                        // the loop start's decoder value. FIX: FUN_1004aad0 sets both +0x28 (block +0x10) and +0x34
                        // (block +0x14), also for a mono voice (+0x34 = checkpoint 1, unused here but kept by the slot)
                        y0_[0] = s_->checkpoint[0];
                        y0_[1] = s_->checkpoint[1];
                        rateLn_ += loopPitchDelta_;
                        loopPitchDelta_ = 0;
                    }
                }
                for (int c = 0; c < ch_; ++c) slope_[c] = step_ ? s_->delta[pos_ + c] : 0.f;
                pos_ += step_;
                continue;
            }
            if (pos_ == end_) {
                if (loopLen_ == 0) {
                    pos_ = end_ - step_;
                    step_ = 0;
                } else {
                    pos_ -= loopLen_;
                    rateLn_ += loopPitchDelta_;
                    loopPitchDelta_ = 0;
                }
            }
            for (int c = 0; c < ch_; ++c) {
                y0_[c] += slope_[c];
                slope_[c] = data[pos_ + c] - y0_[c];
            }
            pos_ += step_;
        }
        if (quiet) {
            phase_ = float(double(phase_) + double(inc_));
            continue;
        }
        double o[2] = {0.0, 0.0};
        for (int c = 0; c < ch_; ++c) {
            const double x = double(slope_[c]) * double(phase_) + double(y0_[c]);
            Svf& s = f_[c];
            const double hp = ((double(kk_) * double(s.bp) - x) - double(s.xprev)) + double(s.lp);
            s.hp = float(hp);
            const double bp = double(s.bp) - hp * double(a_);
            s.bp = float(bp);
            s.lp = float(bp * double(b_) + double(s.lp));
            s.xprev = float(x);
            const double t = double(s.t) - ((double(s.t) - double(s.lp)) + double(s.u)) * double(cc_);
            s.t = float(t);
            s.u = float(t * double(cc_) + double(s.u));
            double v;
            switch (tap_) {
                case kLp: v = s.lp; break;
                case kBp: v = s.bp; break;
                case kHp: v = s.hp; break;
                case kU: v = s.u; break;
                case kT: v = s.t; break;
                default: v = 0.0; break;
            }
            if (invert_) v = (double(s.xprev) + double(s.xprev)) - v;
            o[c] = v;
        }
        if (ch_ == 1) o[1] = o[0];
        phase_ = float(double(phase_) + double(inc_));
        const double gl = double(gainL_) + double(incL_);
        gainL_ = float(gl);
        out[2 * n] = float(gl * o[0] + double(out[2 * n]));
        const double gr = double(gainR_) + double(incR_);
        gainR_ = float(gr);
        out[2 * n + 1] = float(gr * o[1] + double(out[2 * n + 1]));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Sample oscillator (render modes 0x10..0x17): the defaults of the oscillator hooks. Note-on case 2 (@10016260).
void Voice::startOsc(const VoiceStart& vs, float startMod, MsvcRand& rng) {
    // start point: one of 64 seek checkpoints, quadratically spaced
    int idx;
    {
        // FIX: + Leg>Start (+0x86c) when >= 2 notes are held and the element is mono (@100162a0)
        const double x = (((double(rng.bi()) * double(e_->randStart) * 0.5 + double(e_->start)) -
                           double(e_->velToStart) * double(vel_)) + double(startMod)) +
                         (vs.monoLegato ? double(e_->legStart) : 0.0);
        idx = int(x * 63.0);  // fistp, chop
    }
    const uint32_t frames = std::min<uint32_t>(z_->frames ? z_->frames : s_->frames, s_->frames);
    int64_t startFrames = 0;
    // FIX: the compressed branch seeks only for idx > 0 (@100163b6 jle), the PCM branch for any idx != 0
    // (@100164c5 test/je): a negative index is clamped only from above and squared, so it seeks as well.
    if (s_->format == 1 ? idx > 0 : idx != 0) {
        idx = std::min(idx, 63);
        const int64_t i2 = int64_t(idx) * idx;
        startFrames = (frames & 0xffff0000u) ? int64_t(frames >> 12) * i2 : (int64_t(frames) * i2) >> 12;
        if (s_->format != 1) startFrames -= startFrames & 1;  // FIX: only the PCM path (@100164e6) rounds to even
    }
    pos_ = s_->format == 1 ? startFrames * s_->channels : startFrames;  // PCM indexes samples with the frame value
    end_ = int64_t(frames) * s_->channels;
    loopLen_ = int64_t(z_->loopLen) * s_->channels;
    if (pos_ >= end_) pos_ = std::max<int64_t>(0, end_ - s_->channels);
    // zone volume and pan (@1001652a)
    levelMod_ = z_->volume * levelMod_;
    panMod_ = z_->pan + panMod_;

    if (z_->rootFlags & 0x200) {
        // FIX: x87 order (@10016586): +0x21c = float(ln((float)(1/sr) * rate)); Formant +12 (+0x860 == 12): +0x21c =
        // float(ln - Keytrack * note pitch) with the root key at 60 - patch transpose; else the root from the element
        // key range. Then ((60 - r) * Keytrack + tune) * 0.057762265f + the STORED +0x21c, one rounding.
        const double L = std::log(double(1.f / c_.sr) * double(s_->rate));
        float base = float(L);
        int r = 60;
        if (e_->formant == 12) {
            base = float(L - double(e_->keytrack) * double(notePitch_));
            r = 60 - vs.transpose;
        } else if (e_->keyHi - e_->keyLo < 24) {
            r = ((e_->keyHi - e_->keyLo) >> 1) + e_->keyLo;
        } else {
            if (e_->keyLo > 48) r = e_->keyLo + 12;
            if (e_->keyHi < 72) r = e_->keyHi - 12;
        }
        rateLn_ = float((double(60 - r) * double(e_->keytrack) + double(z_->tune)) * double(0.057762265f) +
                        double(base));
    } else {
        // FIX: x87 at 53 bits: ln((float)(1/sr) * rate) kept in the register, (int + tune) * 0.057762265f added,
        // one rounding to float at the end (FUN_10014910 @10016573..10016675)
        const double L = std::log(double(1.f / c_.sr) * double(s_->rate));
        rateLn_ = float((double(60 - z_->root()) + double(z_->tune)) * double(0.057762265f) + L);
    }
    loopPitchDelta_ = z_->loopTune * kSemitone;
    quietSkip_ = s_->format == 1 && ch_ == 2 && e_->filterMode != 1 && e_->filterMode != 3;  // render mode 0x14
    maxInc_ = 16.f;
    phase_ = 0;
    // FIX: only the compressed branch clears the interpolation words (+0x28/+0x2c, +0x34/+0x38); a PCM note starts
    // from the slot's previous values (the first segment blends them in)
    if (s_->format == 1) y0_[0] = y0_[1] = slope_[0] = slope_[1] = 0;
    // FIX: compressed checkpoint restores the decoder value (seek table, @100163f5): +0x28 = word ch*idx of the
    // table, +0x34 = the word after it, whatever the channel count (mono: checkpoint idx + 1; a later note in this
    // slot blends in these words)
    if (s_->format == 1 && idx > 0) {
        y0_[0] = s_->checkpoint[size_t(idx) * s_->channels];
        y0_[1] = s_->checkpoint[size_t(idx) * s_->channels + 1];
    }
    // FIX: a mono legato note (>= 2 held) restarts the amp attack with Leg>Attack (FUN_1004a340(4, -1), @10016440)
    if (vs.monoLegato) nextStage(aenv_, true, -1);
}

// FUN_1004ab60 @1004adda: inc = exp(rateLn + pitch), at most maxInc (the word +0x44 holds pitch, then inc)
void Voice::oscControl() {
    const double r = x87exp(double(rateLn_) + double(pitch_));
    inc_ = r > double(maxInc_) ? maxInc_ : float(r);
}

bool Voice::oscFinished() const { return step_ == 0 && std::fabs(f_[0].bp) + std::fabs(f_[0].lp) < 1e-4f; }

}  // namespace hs2
