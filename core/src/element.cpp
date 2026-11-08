#include "hs2/element.h"
#include "hs2/element_types.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "hs2/dsp.h"

namespace hs2 {

namespace {
float sustainOf(float v) { return v < 1e-8f ? 1e-8f : std::min(v, 0.99f); }
float fadeOf(float v) {  // setter cases 0x48 / 0x57
    if (v >= 0.52f) return float(expF(float(15.0 - double(v) * 20.0)));
    if (v > 0.48f) return 0.f;
    return float(-expF(float(double(v) * 20.0 - 5.0)));
}

// FUN_1001c570: precompute one mod-matrix row. param in -1..1, depth in -1..1.
void setupModSlot(ModSlot& s, float param, float depth, int etype, const EngineConsts& c, float tempoToRates) {
    // x87 order: register values double, one rounding at each store (verified against the disassembly).
    // (part+0x954 = 120 / host bpm - 1) * Tempo to Env Rates (elem+0x544) + 1 (@1001c58a)
    const float tempo = float((double(c.tempoFactor) - 1.0) * double(tempoToRates) + 1.0);
    const float T = c.blockT;
    float w5 = 0, w6 = depth, w7 = 0;
    int w8 = 0;
    // blockT / t(v) per block, 1 below t = 1 ms (t kept unrounded)
    auto perBlock = [&](double t) { return t < double(0.001f) ? 1.f : float(double(T) / t); };
    const float half = (param + 1.f) * 0.5f;  // (param + 1) / 2, stored as the float argument of FUN_10048da0
    if (s.mod < 0x1d) {
        w5 = float((param > 0.f ? 1.0 - double(param) : 1.0) * double(depth));
        w6 = param * depth;
        bool skipZeroMod = false;
        if (s.dest == 5) {  // Amp is multiplicative
            w8 = 99;
            if (s.mod != 0) skipZeroMod = true;
            else if (s.src != 8 && s.src > 5 && s.src < 0x14) w8 = 100;  // MIDI controllers: bipolar
        }
        if (!skipZeroMod && s.mod == 0) {
            w5 = depth;
            w6 = 0;
        }
    } else {
        const int k = s.mod - 0x1c;
        w5 = param;
        w6 = depth;
        w8 = k;
        const double P = param;
        switch (s.mod) {
            case 0x1d: case 0x1e: case 0x1f: w5 = float(P * P * P * 10.0); break;             // Add, Multiply, Drive
            case 0x20: if (param < 0.f) w8 = 0x61; break;                                    // Clip+
            case 0x21: if (param > 0.f) w8 = 0x62; break;                                    // Clip-
            case 0x22: case 0x23: case 0x27: case 0x28: case 0x39: break;                     // Rect+/-, Log, Switch, X-Fade
            case 0x24: if (param < 0.f) w5 = param * 0.5847f; break;                          // Curve
            case 0x25: w5 = param * 25.132742f; break;                                       // Sine
            case 0x26: w5 = float(std::fabs(P) * P * 10.0); break;                           // Exp
            case 0x29: case 0x2a: {                                                          // Quant 1/2
                const double f = (P + 1.0) * 0.5;
                const int n = 1 - int(f * f * double(-30.9f));  // fistp (chop) of the negated product
                w8 = (n & 1) + 0xd;
                w5 = float(n) * 0.5f + ((n == 1 || k == 0xe) ? -0.001f : 0.499f);
                w7 = 2.f / float(n);
                break;
            }
            case 0x2b:                                                                         // S+H
                w5 = perBlock(timeCurveD(half) * tempo);
                if (s.src == 0) s.src = 99;  // sample the audio output
                break;
            case 0x2c: w5 = float(timeCoefD(float(timeCurveD(half)), T) * double(0.1f)); break;  // Drift
            case 0x2d: case 0x2e:                                                              // Sine Amp/Freq
                w7 = 0;
                w5 = float(expF(float(P * double(2.3025851f))) * double(T) * 6.2831854820251465);
                break;
            case 0x2f: case 0x30: case 0x33: case 0x34:                                      // Lin/Delay Ramp, Tri/Gate Env
                if (param < 0.f) w8 = s.mod + 0x48;
                w5 = perBlock(timeCurveD(std::fabs(param)) * tempo);
                if (k == 0x14 || k == 0x17) w5 *= 0.5f;
                break;
            case 0x31:                                                                         // Exp Ramp
                if (param < 0.f) w8 = s.mod + 0x48;
                w5 = float(timeCoefD(float(timeCurveD(std::fabs(param)) * tempo), T));
                break;
            case 0x32: {                                                                       // Log Ramp
                if (param < 0.f) w8 = s.mod + 0x48;
                const float a = T * 6.9077554f;
                w5 = float(expF(float(double(a) / timeCurveD(std::fabs(param)))));
                w7 = 0.001f;
                break;
            }
            case 0x35: w5 = float(timeCoefD(float(timeCurveD(half)), T)); break;            // Log Lag
            case 0x36: w5 = perBlock(timeCurveD(half) * tempo); break;                       // Lin Lag
            case 0x37: case 0x38:                                                              // Peak/Diff Hold
                w5 = param <= 0.99f ? perBlock(timeCurveD(half)) : 0.f;
                break;
            default: w8 = 0; break;
        }
    }
    // destination units: pitch-like dests in octaves (ln2), cutoff x 20 octaves unless the source is Key/Pitch
    switch (s.dest) {
        case 1: case 7: case 8: case 9:
            if (s.dest == 9 && etype != 3) break;
            w6 *= 0.6931471f;
            if (s.mod <= 0x1c) w5 *= 0.6931471f;
            break;
        case 3:
            if (s.src == 4 || s.src == 5) break;
            w6 *= 13.862944f;
            if (s.mod <= 0x1c) w5 *= 13.862944f;
            break;
        default: break;
    }
    s.w1 = w5;
    s.w2 = w6;
    s.w6 = w7;
    s.special = w8;
}

}  // namespace

// FUN_10048b80: unity between lo..hi, linear ramps of 10000/(1+trunc(xf^2*127.9)) per step outside.
void buildXfade(std::array<int16_t, 128>& t, float lo, float hi, float xf) {
    const int a = quant(lo, 127.9f);
    const int b = 1 + quant(hi, 127.9f);  // one past the top
    const int step = 10000 / (1 + quant(xf * xf, 127.9f));
    auto clip = [](int v) { return int16_t(v < 1 ? 0 : v); };
    if (b < a) {  // split range: unity below b and from a up, crossfade valley between
        for (int i = 0; i < 128; ++i) t[i] = 10000;
        for (int i = b, up = (b - a) * step + 10000, down = 10000 - step; i < a; ++i, up += step, down -= step)
            t[i] = clip(std::max(up, down));
        return;
    }
    for (int i = 0; i < 128; ++i) t[i] = 10000;
    for (int i = 0, v = 10000 - step * a; i < a && i < 128; ++i, v += step) t[i] = clip(v);
    for (int i = b, v = 10000 - step; i < 128; ++i, v -= step) t[i] = clip(v);
}
// Patch Hyperknob macros (FUN_1000cc40): knob i adds (value - ofs) * rng to macro slot `assignment`.
std::array<float, 16> computeMacros(const ParamArray& pp) {
    struct Knob { int value, ofs, rng, assign; };
    static constexpr Knob kKnobs[6] = {{0x0e, 0x71, 0x72, 0x70}, {0x0f, 0x74, 0x75, 0x73}, {0x67, 0x77, 0x78, 0x76},
                                       {0x69, 0x8b, 0x8c, 0x8a}, {0x6b, 0x8e, 0x8f, 0x8d}, {0x6d, 0x7c, 0x7d, 0x7b}};
    std::array<float, 16> off{};
    for (const auto& k : kKnobs) {
        const int slot = quant(pp[k.assign], 15.9f) - 1;  // -1 = (none)
        if (slot >= 0 && slot < 16) off[slot] = (pp[k.value] - pp[k.ofs]) * pp[k.rng];
    }
    return off;
}

// FUN_1000ca70 weights (part+0x8d0 + slot*4): patch setter cases 0x7e..0x87 ("GM" params, reached by MIDI CC 70..79)
// store 2v - 1 for slots 0..9 (0x81 first maps v -> 0.6v + 0.2 when part+0x114c is set, never in this port).
std::array<float, 16> computeMacroWeights(const ParamArray& pp) {
    std::array<float, 16> w{};
    for (int k = 0; k < 10; ++k) w[k] = float(double(pp[0x7e + k] + pp[0x7e + k]) - 1.0);
    // FIX: slot 11 (Glide Time, element 0x4e) has its own setter case: 0x94 (MIDI CC 5, Portamento Time) stores
    // part+0x8fc = 2v - 1 (@10012ea5; v*0.5 - 1 with part+0x114c, never in this port). Every factory patch stores 0.5
    // (weight 0), so only CC 5 moves it. Slots 10, 12..15 are only ever 0.
    w[11] = float(double(pp[0x94] + pp[0x94]) - 1.0);
    return w;
}

// FUN_1001c440 (k >= 0): v' = (1 - w^2) * clamp(v + knob, 0, 1) + (w > 0 ? w^2 : 0), in the x87 registers.
double applyMacroD(float v, float knob, float w) {  // the x87 register value FUN_1001c440 returns
    double x = double(v) + double(knob);
    if (x < 0.0) x = 0.0;
    else if (x > 1.0) x = 1.0;
    const double ww = double(w) * double(w);
    return (1.0 - ww) * x + (w > 0.f ? ww : 0.0);
}
float applyMacro(float v, float knob, float w) { return float(applyMacroD(v, knob, w)); }

namespace {
// Element quick knobs A/B (params 0x0e/0x0f, modes 0x04/0x05) routed to up to 4 params (FUN_1001c440, k < 0).
void applyElementKnobs(ParamArray& q, const ParamArray& p, const ParamArray& pp, bool ownKnobs = false) {
    auto shaped = [](float v, int mode) {
        switch (mode) {
            case 1: return v - 1.f;               // Negative
            case 2: return v - 0.5f;              // Bipolar
            case 3: return v <= 0.5f ? 0.f : 1.f; // Switch
            default: return v;                    // Positive
        }
    };
    // FIX: knob A/B take the value of patch Hyperknob "1./2. Source" (0x91/0x92: trunc(v*7.9) = 1..6) when set;
    // the patch setter (FUN_100121a0 cases 0x0e/0x0f/0x67/0x69/0x6b/0x6d) forwards it to element param 0x0e/0x0f.
    static constexpr int kPatchKnob[6] = {0x0e, 0x0f, 0x67, 0x69, 0x6b, 0x6d};
    auto knobValue = [&](int own, int srcParam) {
        const int src = quant(p[srcParam], 7.9f);
        return !ownKnobs && src >= 1 && src <= 6 ? pp[kPatchKnob[src - 1]] : p[own];
    };
    const float knob[2] = {shaped(knobValue(0x0e, 0x91), quant(p[0x04], 3.9f)),
                           shaped(knobValue(0x0f, 0x92), quant(p[0x05], 3.9f))};
    // FIX: FUN_1001c440 (k < 0) sums v + (2 depth - 1) * knob for route bits 1, 2, 4, 8 in the x87 registers and
    // clamps; the setter stores the result once as float (82/15 Crackles: level one ulp off)
    double acc[256];
    bool hit[256] = {};
    for (int s = 0; s < 4; ++s) {
        const int dest = 0x10 + quant(p[0x06 + 2 * s], 235.9f);
        if (dest == 0x10 || dest > 0xff) continue;  // 0x10 = OFF
        if (!hit[dest]) acc[dest] = q[dest];
        acc[dest] = (double(p[0x07 + 2 * s]) + double(p[0x07 + 2 * s]) - 1.0) * double(knob[s / 2]) + acc[dest];
        hit[dest] = true;
    }
    for (int i = 0; i < 256; ++i)
        if (hit[i]) q[i] = float(std::clamp(acc[i], 0.0, 1.0));
}
}  // namespace

// The routes with the element's own knob values 0x0e / 0x0f: what the element's load pass sees, before the loader's
// final Hyperknob calls hand the knobs the Hyperknob values (setter cases that read another word use these).
ParamArray elementOwnKnobParams(const ParamArray& raw, const ParamArray& pp) {
    ParamArray p = raw;
    applyElementKnobs(p, raw, pp, true);
    return p;
}

ParamArray elementKnobParams(const ParamArray& raw, const ParamArray& pp) {
    ParamArray p = raw;
    applyElementKnobs(p, raw, pp);
    return p;
}

ElementParams computeElement(const ParamArray& raw, const ParamArray& pp, const EngineConsts& c) {
    ElementParams e;
    ParamArray p = raw;
    applyElementKnobs(p, raw, pp);
    float lfo3RateParam = p[0x57];
    // FIX: cases 0x40 (Cutoff), 0x44 (Filter Env amount) and 0x81 (Formant) use FUN_1001c440's result in the x87
    // register, unrounded (13/64: Cutoff one ulp off)
    double cutD = p[0x6a], fenvD = p[0x6e], formD = p[0x16];
    static const bool kNoMacros = std::getenv("HS2_NOMACRO") != nullptr;  // debug switch for A/B experiments
    if (raw[0x02] > 0.5f && !kNoMacros) {  // Receive CC/EE: patch macros (FUN_1001c440, k >= 0; GM weights not modelled)
        const auto off = computeMacros(pp);
        const auto wt = computeMacroWeights(pp);
        const bool sample = quant(raw[1], 15.9f) == int(ElementType::Sample);
        struct Kind { int param, kind; };
        static constexpr Kind kKinds[] = {{0x62, 0}, {0x57, 1}, {0x6a, 2}, {0x68, 3}, {0x6e, 4}, {0x6f, 5},
                                          {0x70, 6}, {0x73, 6}, {0x7e, 7}, {0x7f, 8}, {0x82, 9}, {0x43, 10},
                                          {0x4e, 11}, {0x80, 13}, {0x71, 13}};
        for (const auto& k : kKinds) {
            if (k.param == 0x62 && sample) continue;  // Sample "Wave" is stored raw
            const double d = applyMacroD(p[k.param], off[k.kind], wt[k.kind]);
            if (k.param == 0x6a) cutD = d;
            if (k.param == 0x6e) fenvD = d;
            p[k.param] = float(d);
        }
        if (sample) {  // Drum Sel / Formant
            formD = applyMacroD(p[0x16], off[0], wt[0]);
            p[0x16] = float(formD);
        }
        // FIX: case 0x2d applies the LFO Rate macro (FUN_1001c440 kind 1) when part+0x960 is set: patch "LFO Rate Ctl"
        // (0x46 -> part+0xbc4 = trunc(v*7.9)) bit 2 (bits 0/1 are the patch LFO1/2, PatchPlayer::load).
        if (quant(pp[0x46], 7.9f) & 4) lfo3RateParam = applyMacro(lfo3RateParam, off[1], wt[1]);
    }
    e.mute = p[0] > 0.5f;
    e.type = ElementType(quant(p[1], 15.9f));
    e.version = int(e.type) + 1;
    e.click = p[0x7d];
    // type-specific block for Sample (idx 0x12 Bank, 0x13 Wave)
    e.bank = quant(p[0x12], 128.9f);
    e.wave = quant(p[0x13], 128.9f);
    e.triggerCond = quant(p[0x37], 15.9f);
    {  // element setter case 3 (ECO/XXL) with the part quality (patch param 1 -> part+0x94c, trunc(v*2.9))
        const int quality = quant(pp[0x01], 2.9f);
        e.eco = quality == 0 || (quality != 2 && p[0x03] <= 0.5f);
    }
    e.keyLo = quant(p[0x31], 127.9f);
    e.keyHi = quant(p[0x32], 127.9f);
    e.velLo = quant(p[0x34], 127.9f);
    e.velHi = quant(p[0x35], 127.9f);
    e.receiveVelocity = p[0x8a] > 0.5f;
    buildXfade(e.keyGain, p[0x31], p[0x32], p[0x33]);
    buildXfade(e.velGain, p[0x34], p[0x35], p[0x36]);
    e.undampedKey = quant(p[0x94], 128.9f);
    if (e.undampedKey == 0) e.undampedKey = 128;
    // Element setter (FUN_100221f0) values, computed as the x87 does: one rounding to float at the store.
    e.noteDelayMode = quant(p[0x3f], 7.9f);
    e.fadeTime = p[0x40];
    e.velToDelay = float(double(p[0x41]) * 3.0 - 1.5);  // case 0x17
    e.glideMode = quant(p[0x4c], 3.9f);
    e.glideAnalog = p[0x4d] > 0.5f;
    e.glideCoef = float(timeCoefD(float(timeCurveD(p[0x4e])), c.blockT));  // case 0x24
    e.velToGlide = float((double(p[0x4f]) - 0.5) * double(6.931472f));    // case 0x25
    auto bend = [](float v) {  // cases 0x26 / 0x28: v - 0.5 stays in the register
        const double x = double(v) - 0.5;
        return float(std::fabs(x) * x * double(8.317766f));
    };
    e.attackBend = bend(p[0x50]);
    e.velToBend = p[0x51] + p[0x51] - 1.f;
    e.releaseBend = bend(p[0x52]);
    // Mono unison terms (cases 0x12..0x14; the note-on uses them only inside a unison stack)
    {
        const int n = quant(p[0x3c], 7.9f);
        e.unisonInv = n ? float(1.0 / double(float(n))) : 0.f;
        e.unisonDetune = float(double(p[0x3d]) * double(p[0x3d]) * double(p[0x3d]) * double(kSemitone));
        e.unisonPan = p[0x3e] + p[0x3e];
    }
    e.receiveCC = raw[0x02] > 0.5f;
    e.patchGlide = quant(pp[0x03], 3.9f);
    // FIX: voice allocation and mono (setter cases 0x0e..0x12, 0x29/0x2a, 0x65), for every element type.
    e.muteCond = quant(p[0x38], 15.9f);
    e.monoMode = quant(p[0x39], 4.9f);
    if (e.receiveCC) {  // case 0x39: the patch Mono/Poly (0x39 -> part+0x948, trunc(v*2.9)) overrides the mode
        const int pm = quant(pp[0x39], 2.9f);
        if (pm == 0) e.monoMode = 0;
        else if (pm == 2 && e.monoMode == 0) e.monoMode = 4;
    }
    e.maxVoices = 1 + quant(raw[0x3a], 63.9f);  // case 0x10 uses the stored (not quick-knob routed) value
    e.envResume = p[0x3b] > 0.5f;
    e.unisonVoices = quant(p[0x3c], 7.9f);
    e.legatoRetrig = quant(p[0x53], 4.9f);
    e.releaseRetrig = quant(p[0x54], 4.9f);
    {
        const int pr = quant(p[0x90], 2.9f);
        e.priority = pr == 0 ? 0 : pr == 1 ? 1 : 6;
    }
    if (quant(raw[1], 15.9f) == int(ElementType::Sample)) {  // Sample cases 0x85 / 0x86 (other types reset them)
        e.legAttack = float(double(p[0x1d]) * double(p[0x1d]) * 0.25);
        e.legFAttack = float(double(p[0x1e]) * double(p[0x1e]) * 0.25);
        e.legStart = p[0x1c];
        e.forceMono = p[0x14] > 0.5f;  // case 0x80
    }
    e.randDelay = p[0x42];

    const int octave = (quant(p[0x46] * 8.f + 0.5f, 1.f) - 4) * 12;
    const int semis = quant(p[0x47] * 24.f + 0.5f, 1.f) - 12;
    e.octaveSemis = octave;
    e.semis = semis;
    e.zoneVelMode = quant(p[0x11], 15.9f);
    e.formant = int(formD * 24.0 + 0.5) - 12;  // case 0x81 (x87)
    e.formantVel = p[0x17] * 0.381f - 0.1905f;
    e.formantRand = p[0x18] * 12.f;
    e.velOffs = quant(p[0x21] * 256.f + 0.5f, 1.f) - 0x40;
    e.velScale = p[0x22] * 4.f - 1.f;
    e.velRnd = p[0x23] * 64.f;
    {  // FUN_1001c410: ((semis + octave) + cents) * ln2/12; cents (case 0x1e) = 2 |v - 0.5| (v - 0.5)
        const double d = double(p[0x48]) - 0.5;
        const float cents = float(std::fabs(d) * d * 2.0);
        e.pitchBase = float((double(float(semis + octave)) + double(cents)) * double(kSemitone));
    }
    e.keytrack = p[0x49] * 4.f - 1.f;
    e.randomPitch = float(double(p[0x4a]) * p[0x4a] * p[0x4a] * p[0x4a] * double(0.6931471f));  // case 0x20

    e.start = p[0x43];
    e.velToStart = p[0x44] * 2.f - 1.f;
    e.randStart = p[0x45];
    e.wave5c0 = p[0x62];
    e.keyToWave = p[0x63] * 2.f - 1.f;
    e.velToWave = float(double(p[0x64]) * 3.0 - 1.5);  // case 0x3a
    e.randWave = p[0x65];                              // case 0x3b

    e.filterMode = quant(p[0x67], 15.9f);
    e.reso = p[0x68];
    e.velToReso = p[0x69] * 2.f - 1.f;
    e.cutoffLn = float(cutD * double(kTenOctaves) + double(c.lnRef));  // case 0x40
    e.cutoffKey = p[0x6b] * 4.f - 1.f;
    e.velToCutoff = p[0x6c] - 0.5f;
    e.randCutoff = p[0x6d] * p[0x6d] * 0.5f;
    e.fenvAmount = float((fenvD * 2.0 - 1.0) * double(kTenOctaves));  // case 0x44

    auto& f = e.fenv;  // cases 0x45..0x50: times FUN_10048da0, fade / curve exp of a float argument
    f.attack = float(timeCurveD(p[0x6f]));
    f.decay = float(timeCurveD(p[0x70]));
    f.sustain = sustainOf(p[0x71]);
    f.fade = fadeOf(p[0x72]);
    f.release = float(timeCurveD(p[0x73]));
    f.curve = float(expF(float(double(p[0x74]) * 9.0 - 7.0)));
    f.velToLevel = p[0x75] * 4.f - 2.f;
    f.velToAttack = 4.f - p[0x76] * 8.f;
    f.velToDecay = 4.f - p[0x77] * 8.f;
    f.decayKey = 2.f - p[0x78] * 4.f;
    f.relVel = 4.f - p[0x79] * 8.f;
    f.relKey = 2.f - p[0x7a] * 4.f;

    e.tempoToRates = p[0x7b];
    e.muteFadeout = p[0x7c] * p[0x7c];

    auto& a = e.aenv;  // cases 0x54..0x5e
    a.attack = float(timeCurveD(p[0x7e]));
    a.decay = float(timeCurveD(p[0x7f]));
    a.sustain = sustainOf(p[0x80]);
    a.fade = fadeOf(p[0x81]);
    a.release = float(timeCurveD(p[0x82]));
    a.curve = float(expF(float(double(p[0x83]) * 9.0 - 7.0)));
    a.velToLevel = p[0x84] * 4.f - 2.f;
    a.velToAttack = 4.f - p[0x85] * 8.f;
    a.velToDecay = 4.f - p[0x86] * 8.f;
    a.relVel = float(4.0 - (double(p[0x86]) * 0.5 + 0.25) * 8.0);
    a.decayKey = 2.f - p[0x87] * 4.f;
    a.relKey = p[0x89] > 0.01f ? 2.f - p[0x89] * 4.f : a.decayKey * 0.3f;

    // elem+0x5d4 = 2v^2 * (patch block +0x80 = MIDI CC11 expression^2, default 1). Patch Level/Trim are
    // applied later by the part mixer (PatchPlayer::render).
    e.levelRaw = float(double(p[0x8b]) * p[0x8b] + double(p[0x8b]) * p[0x8b]);
    e.level = float(double(e.levelRaw) * double(c.expression));
    e.pitchBendOn = p[0x4b] > 0.5f;
    // FIX: case 0x60 (Output Level): +0x5b8 = v^2 + v^2; a non-effect element below 0.0001 is muted (+0x41c), so
    // FUN_10021e20 starts no voice for it
    if (e.type != ElementType::Effect && float(double(p[0x8b]) * p[0x8b] + double(p[0x8b]) * p[0x8b]) < 0.0001f)
        e.mute = true;

    // LFO3 (per voice) and Env1
    e.lfo3.wave = quant(p[0x55], 15.9f);
    e.lfo3.sync = quant(p[0x56], 4.9f);
    // case 0x2d: +0x4c0 = exp(float(v * 6.8024 - 2.70805)) * blockT, +0x4c4 = noteValue(v) * blockT / 30
    e.lfo3.rate = float(expF(float(double(lfo3RateParam) * double(6.8024f) - double(2.70805f))) * double(c.blockT));
    e.lfo3.phase = p[0x58];
    e.lfo3.tempoRate = float(double(noteValue(lfo3RateParam)) * c.blockT * double(0.033333335f));
    e.env1Start = p[0x59] * 2.f - 1.f;
    for (int i = 0; i < 4; ++i) {  // cases 0x30.. : t(v) * sr * (1/32) + 1 blocks
        e.env1Time[i] = float(timeCurveD(p[0x5a + 2 * i]) * double(c.sr) * 0.03125 + 1.0);
        e.env1Level[i] = p[0x5b + 2 * i] * 2.f - 1.f;
    }

    // Mod matrix rows (setter 0xac.., FUN_1001c570)
    for (int i = 0; i < 16; ++i) {
        ModSlot& s = e.mods[i];
        const float* q = &p[0xac + 5 * i];
        s.src = quant(q[0], 71.9f);
        s.mod = quant(q[1], 71.9f);
        const float param = q[2] * 2.f - 1.f;
        s.dest = quant(q[3], 15.9f);
        const double d = double(q[4]) - 0.5;  // setter: v - 0.5 stays in the register
        const float depth = float(std::fabs(d) * d * 4.0);
        setupModSlot(s, param, depth, int(e.type), c, e.tempoToRates);
    }
    e.modCount = 16;
    while (e.modCount > 0 && e.mods[e.modCount - 1].src + e.mods[e.modCount - 1].mod == 0) --e.modCount;
    e.pan = p[0x8c];
    e.panKey = p[0x8d] * 2.f - 1.f;
    e.randPan = p[0x8e] * 0.5f;
    e.altPan = p[0x8f] * 2.f - 1.f;
    e.typeParams = computeTypeParams(e.type, p, raw, pp, c, e);
    return e;
}

}  // namespace hs2
