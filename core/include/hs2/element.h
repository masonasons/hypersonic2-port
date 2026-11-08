#pragma once
// Element parameters converted to engine units, as Hypersonic's element setter (FUN_100221f0) stores them.
// Field comments give the element-object offset in the original. Currently covers the Sample element path.
#include <cmath>
#include <memory>

#include "hs2/patch.h"

namespace hs2 {

class Content;

struct EngineConsts {
    Content* content = nullptr;     // the user's content folder (Wavetables, Drawbars, ...), set by PatchPlayer
    float sr = 44100.f;
    float blockT = 32.f / 44100.f;  // elem+0xc
    float lnRef = 0.f;              // elem+0x14 = ln(261.62558 / sr)
    // Part words the element setter reads through elem+0xb9c (= part+0x8d0): +0x80 = part+0x950 MIDI expression
    // (CC11: v^2 + 1e-8, 1 by default; Output Level case 0x60 multiplies it in), +0x84 = part+0x954 = 120 / host bpm
    // (Tempo to Env Rates, FUN_1001c570 / FUN_1004a340).
    float expression = 1.f;
    float tempoFactor = 1.f;
    static EngineConsts make(float sr) {  // element setSampleRate (10024fa0): from the unrounded 1/sr
        EngineConsts c;
        c.sr = sr;
        const double inv = 1.0 / double(sr);
        c.blockT = float(32.0 * inv);
        c.lnRef = float(std::log(inv * double(261.62558f)));
        return c;
    }
};

struct EnvParams {
    float attack = 0, decay = 0, sustain = 1, fade = 0, release = 0, curve = 0;
    float velToLevel = 0, velToAttack = 0, velToDecay = 0, decayKey = 0, relVel = 0, relKey = 0;
};

// One mod-matrix row as set up by FUN_1001c570 (element+0x5f8 + 0x24*i).
struct ModSlot {
    int src = 0, mod = 0, dest = 0;  // trunc(v*71.9), trunc(v*71.9), trunc(v*15.9)
    float w1 = 0, w2 = 0;            // normal: scale, offset; processed: parameter, depth
    float w6 = 0;                    // processed modifier state/data (initial value)
    int special = 0;                 // 0 = normal; else FUN_10049090 case
};

struct LfoParams {
    int wave = 0;      // display index: OFF, Constant, Sine, Triangle, Saw, Square, S+H Rnd, S+H Alt, Rnd Drift, Slow Drift
    int sync = 0;      // trunc(v*4.9): 0 Off, 1 First Note, 2 Each Note, 3 Tempo + Note, 4 Tempo + Beat
    float rate = 0;    // per-block phase increment (2.0 = one cycle), free-running
    float tempoRate = 0;  // noteValue(v) * blockT / 30: times bpm = per-block increment when sync >= 3
    float noteVal = 0;    // noteValue(v) (patch LFOs: +0xde8 / +0xdf0, Tempo + Beat phase)
    float phase = 0;   // start phase param (> 0.9 = random)
};

// Per-type element data (Drawbar / Wavetable / FM / Analog): each type defines a struct deriving from this in
// core/src/types/<type>.cpp, filled by its compute<Type>Params (see hs2/element_types.h).
struct TypeParams {
    virtual ~TypeParams() = default;
};

struct ElementParams {
    std::shared_ptr<const TypeParams> typeParams;  // nullptr for Sample / Effect elements
    LfoParams lfo3;                        // +0x470.. (params 0x55..0x58)
    float env1Start = 0;                   // +0x520
    float env1Time[4] = {1, 1, 1, 1};      // +0x524 +0x52c +0x534 +0x53c (blocks)
    float env1Level[4] = {0, 0, 0, 0};     // +0x528 +0x530 +0x538 +0x540
    ModSlot mods[16];
    int modCount = 0;                      // +0x5f4
    ElementType type = ElementType::Effect;
    bool mute = false;
    int bank = 0, wave = 0;                // Sample: bank file number, instrument index
    int triggerCond = 0;  // +0x438 trunc(v*15.9): 0 none, 1/2 mod wheel up/down, 3/4 sustain up/down,
                          // 5/6 odd/even patch note, 7/8 odd/even element note, 9 no arp (FUN_10021e20)
    bool eco = false;  // +0x434: ECO zone set (patch param 1 quality 0 = all ECO, 2 = all XXL, 1 = element 0x03 <= 0.5)
    int keyLo = 0, keyHi = 127, velLo = 0, velHi = 127;
    std::array<int16_t, 128> keyGain{}, velGain{};  // +0xa6c / +0x96c crossfade tables, 10000 = unity (FUN_10048b80)
    bool receiveVelocity = true;           // +0x4ec
    int undampedKey = 128;                 // +0x484: notes >= this ignore note-off
    int noteDelayMode = 0;                 // +0x480 trunc(v*7.9): 3 = "Note Off" (release-triggered)
    // FIX: Mono Mode +0x440 trunc(v*4.9) (0 poly, 1 lowest, 2 highest, 3 first, 4 last note; with Receive CC/EE the
    // patch Mono/Poly 0x39 overrides it, setter case 0x39), Mono Unison Voices +0x450 trunc(v*7.9), every type.
    int monoMode = 0, unisonVoices = 0;
    int legatoRetrig = 0, releaseRetrig = 0;  // +0x444 / +0x448 trunc(v*4.9): 0 Start New Voice .. 4 New Pitch Only
    bool envResume = false;                // +0x44c Envelope Mode "Analog (Resume)" (0x3b)
    int maxVoices = 64;                    // +0x46c Max Voices 1 + trunc(v*63.9) (0x3a, the raw value)
    int priority = 1;                      // +0x47c Priority (0x90): 0 Low, 1 Normal, 6 High
    int muteCond = 0;                      // +0x43c Mute Condition trunc(v*15.9) (0x38)
    float legAttack = 0, legFAttack = 0;   // +0x870 / +0x874: Sample Leg>Attack / Leg>F.Attack v^2/4 (else 0)
    float legStart = 0;                    // +0x86c: Sample Leg>Start (start-point offset of mono legato notes)
    bool forceMono = false;                // +0x858: Sample Force Mono (stereo samples play their left channel)
    bool hasData = true;                   // false: FUN_10021e20 returns 0 (a Drawbar element without its entry)
    // Mono unison stack terms (+0x4a8 = 1/N, +0x4ac = v^3 ln2/12, +0x4b0 = 2v), used by the note-on with param_7
    float unisonInv = 0, unisonDetune = 0, unisonPan = 0;
    float click = 0;                       // +0x4f0 Note-on Click (0x7d, after the loader fixups)
    // +0x5ec parameter version: the render functions re-run FUN_10049eb0 when a voice's copy (+0x520) differs.
    // computeElement leaves a placeholder; PatchPlayer keeps the real count (PatchPlayer::verCall: bumps per setter call,
    // 43 for a new element), so a voice slot moving to an element with another version refreshes as in the original.
    int version = 1;
    // +0x5f0 mod-row version: FUN_1001c570 (row setup) sets it to (v & ~0xff) + 0x100 + row; the part render end
    // loop copies that one row into a voice whose +0x518 differs (FUN_10010c50)
    uint32_t modVersion = 0;
    float fadeTime = 0;                    // +0x4b4 raw (Fade Time / Delay Time)
    float velToDelay = 0, randDelay = 0;
    // Glide (element setter 0x4c..0x52; voice note-on FUN_10014910, control FUN_1004ab60, note-off FUN_10017880)
    int glideMode = 0;          // +0x464 trunc(v*3.9): 0 Off, 1 Bend Only, 2 Fingered, 3 On
    bool glideAnalog = false;   // +0x468: Analog (Resume): note-off freezes the glide where it is
    float glideCoef = 1;        // +0x5dc: per-block approach coefficient (timeCoef of the Glide Time curve)
    float velToGlide = 0;       // +0x4f4 (v - 0.5) * ln(1024)
    float attackBend = 0, velToBend = 0, releaseBend = 0;  // +0x4fc, +0x500, +0x504 (ln pitch)
    bool receiveCC = false;     // +0x428 (0x02): the patch Glide (patch 0x03) can override the legato rule
    int patchGlide = 0;         // part+0xbc0 trunc(patch[0x03]*3.9)   // FIX: +0x4b8 3v-1.5, +0x4bc raw (Note Delay mode 1)
    // zone selection (Sample page, FUN_10021e20)
    int octaveSemis = 0, semis = 0;        // +0x4e4, +0x4e8
    int zoneVelMode = 0;                   // +0x854
    int formant = 0;                       // +0x860 (semitones)
    float formantVel = 0, formantRand = 0; // +0x864, +0x868
    int velOffs = 0;                       // +0x85c
    float velScale = 1, velRnd = 0;        // +0x878, +0x87c
    // pitch
    float pitchBase = 0;                   // +0x5bc (octave/semi/cents, ln units)
    bool pitchBendOn = false;              // +0x454 "Patch Pitch Bend" (0x4b, setter case 0x21): pitch reads part+0xbd4
    float levelRaw = 0;                    // +0x5b8 = v^2 + v^2 (Output Level 0x8b); level = this * expression
    float keytrack = 1;                    // +0x4dc
    float randomPitch = 0;                 // +0x4e0
    // start point
    float start = 0, velToStart = 0, randStart = 0;      // +0x4cc +0x4d0 +0x4d4
    float wave5c0 = 0, keyToWave = 0, velToWave = 0, randWave = 0;  // +0x5c0 +0x508 +0x50c +0x510
    // filter
    int filterMode = 0;                    // +0x478
    float reso = 0, velToReso = 0;         // +0x5c4 +0x514
    float cutoffLn = 0, cutoffKey = 0;     // +0x5c8 +0x5cc
    float velToCutoff = 0, randCutoff = 0; // +0x518 +0x51c
    float fenvAmount = 0;                  // +0x5d0
    EnvParams fenv, aenv;                  // +0x548.. / +0x578..
    float tempoToRates = 0;                // +0x544
    float muteFadeout = 0;                 // +0x5a8
    // output
    float level = 0;                       // +0x5d4 (2v^2 * patch level)
    float pan = 0.5f;                      // +0x5d8
    float panKey = 0, randPan = 0, altPan = 0;  // +0x5ac +0x5b0 +0x5b4
};

ElementParams computeElement(const ParamArray& p, const ParamArray& patchParams, const EngineConsts& c);
// FIX: raw element params after the quick-knob routing (FUN_1001c440 k < 0); effects take their knobs from this.
ParamArray elementKnobParams(const ParamArray& raw, const ParamArray& patchParams);
ParamArray elementOwnKnobParams(const ParamArray& raw, const ParamArray& patchParams);  // knobs = own 0x0e / 0x0f
std::array<float, 16> computeMacros(const ParamArray& patchParams);
std::array<float, 16> computeMacroWeights(const ParamArray& patchParams);
float applyMacro(float v, float knob, float weight);
// FUN_10048b80: key / velocity crossfade table (unity 10000 inside lo..hi, ramps of 10000 / (1 + trunc(xf^2 * 127.9)))
void buildXfade(std::array<int16_t, 128>& t, float lo, float hi, float xf);

}  // namespace hs2
