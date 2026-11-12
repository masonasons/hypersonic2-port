#pragma once
// Voice: the shared voice engine (note-on FUN_10014910, control update FUN_1004ab60, envelopes FUN_1004a340, mod
// matrix FUN_1001c570 / FUN_10049090, LFOs, glide, filter coefficients) with the Sample element's oscillator as the
// default. Other element types (Drawbar, Wavetable, FM, Analog: render modes 1/3/4/5/-2..-4 in FUN_100139c0) derive
// from Voice in core/src/types/<type>.cpp and override the oscillator hooks (startOsc / oscControl / renderOsc /
// oscFinished), or the filter stage where their original differs. See docs/engine/sample_voice.md and ARCHITECTURE.md.
//
// Precision: the original runs the x87 at 53 bits. This code mirrors it: register values are doubles, rounded to
// float only where the original stores a word (see hs2/dsp.h x87exp etc.). Free-running oscillators turn any
// rounding difference on the pitch path into phase drift, and long envelopes lose digits in float.
#include "hs2/dsp.h"
#include "hs2/element.h"
#include "hs2/samples.h"

namespace hs2 {

// LFO oscillator (FUN_10049a60 init / FUN_10048e10 tick), x87-exact. Phase runs 0..1 per half-cycle.
// Layout as the original's object: +0 phase, +4 value, +8 slope, +0xc wave, +0x10 half.
struct Lfo {
    float phase = 0, value = 0, slope = 0;
    int wave = 0;  // internal numbering (display 0->1, 1->2, 2->0)
    int half = 0;
    float init(float phase01, int displayWave, MsvcRand& rng);  // returns the start value
    float tick(float rate, MsvcRand& rng);                       // returns the new value
private:
    void driftSlope(MsvcRand& rng);
};

// Part words every voice reads (mod sources FUN_10010930, pitch pointer). PatchPlayer::midi() keeps them current.
struct PatchInputs {
    // sources 6 mod wheel (part+0xda4, = +0xda0 copied at each part render call), 7 aftertouch (+0xdac, smoothed),
    // 8 pitch bend (+0xbcc, -1..1), 9 breath CC2 (+0xdb0), 10 CC3 (+0xdb4), 11 foot CC4 (+0xdb8)
    float modWheel = 0, aftertouch = 0, pitchBend = 0, breath = 0, wheel3 = 0, foot = 0;
    float knob[6] = {};
    float scale2 = 0;          // unused: source 20 is per voice (Voice::scale2_, set at note-on)
    float seq1 = 0, seq2 = 0;  // sources 21 (+0xdc0, or +0x970 Piano Character - 0.375 in patch type 2), 22 (+0xdc4)
    float lfo1 = 0, lfo2 = 0;
    // voice pitch pointer v[0x93] (FUN_10014910 @10015807): +0xbd4 = bend semitones * ln2/12 + fine tune for elements
    // with "Patch Pitch Bend" (0x4b) on, else +0xbc8 = the fine tune alone
    float pitchOfs = 0, fineTune = 0;
    float tempoFactor = 1;  // part+0x954 = 120 / bpm (FUN_1000ef10)
    // FIX: part+0xdd4, the patch LFO1/2 values of the current part render call as (lfo1, lfo2) pairs, one pair per
    // tick (FUN_100139c0). A voice reads the next pair at each control update (voice+0x2e4, reset at render entry).
    const float* lfoArr = nullptr;
};

// Filter env (3) and amp env (4) share this machine (FUN_1004a340).
struct ExpEnv {
    float level = 0, coef = 1, target = 0, hi = 256, lo = -256, peak = 1;
    int stage = 0;
};

struct VoiceStart {
    float bpm = 120.f;       // part+0xd90 (tempo-synced LFO3)
    double bps = 120.0 / 60.0 / 44100.0;  // part+0xd88 beats per sample (Note Delay mode 2)
    float* lastGlide = nullptr;  // element+0x4f8: previous note's glide target (legato start)
    bool otherHeld = false;      // another note is held (Fingered glide)
    int note = 60;
    int transpose = 0;       // FIX: patch Transpose (part+0xbb4)
    int velocity = 100;      // 1..127
    int cents = 0;           // FUN_10014910 param_2: the note's detune (part+0x120+note = MIDI byte 3; arp: byte 3 * 100)
    double tune = 0;         // FUN_10010870(note): microtuning in cents (x87 register value), added to the note pitch
    float scale2 = 0;        // FUN_10010870's second result: 0.01 * tuning 2 cents -> voice+0x310 (mod source 20)
    float sustainWord = 0, softWord = 0;  // +0x304 = (float)part+0xd98, +0x308 = part+0xdbc (sources 12 / 13)
    float noteGain = 1.f;    // key/velocity crossfade gain
    uint32_t altCounter = 0; // FIX: alternate pan: the ELEMENT's note counter (elem+0xb6c, FUN_10021e20)
    int unisonIndex = 0;     // FUN_10014910 param_7: 1..N+1 in a mono unison stack, else 0 (kept at +0x52c)
    float drawbarOther = -1.f;
    bool monoLegato = false;    // part+0x1c >= 2 notes held and the element is mono (Sample: Leg>Start / Leg>Attack)  // Drawbar percussion: max level (+0x164) of the other held drawbar voices, else -1
    // Sample elements: the selected zone and its decoded sample (nullptr for other element types)
    const Zone* zone = nullptr;
    const SampleData* sample = nullptr;
};

class Voice {
public:
    virtual ~Voice() = default;
    bool active() const { return active_; }
    // The decoded sample this voice last played (sample voices; kept after the voice ends until it plays again).
    const SampleData* lastSample() const { return s_; }
    int note() const { return note_; }

    void start(const ElementParams& e, const EngineConsts& c, const VoiceStart& vs, const PatchInputs& in,
               MsvcRand& rng);
    virtual void release();
    // FUN_100101a0 (voice steal / mono release): the amp env goes to its stage 5, the Voice Mute Fadeout (+0x534 = 2).
    void steal();
    // FUN_10016820: mono legato / Envelope Mode "Analog (Resume)" retrigger of this voice with a new note. mode =
    // Mono Legato / Release Retrig (0 = full, 1 All Modulation, 2 Filt+Amp Env, 3 Filt Env, 4 New Pitch Only).
    void legato(int note, int transpose, int velocity, int mode, int patchGlide, MsvcRand& rng, int cents = 0,
                float sustainWord = 0, float softWord = 0, double tune = 0);
    void setSustainWord(float v) { sus_ = v; }
    void setReleaseKey(int key) { relKey_ = key; }  // the note byte the next release() tests against Undamped >
    void clearReleased() { released_ = false; }      // held again by the sustain pedal (Redamper)  // CC64 (FUN_10017880 @10017c90): +0x304 of every listed voice
    // CC64 down with Redamper (part+0xbf0): a released voice is held again and its amp env coefficient (+0x2a0)
    // scaled by g (@10017d55: float(g * coef))
    void scaleAmpCoef(float g) { aenv_.coef = float(double(g) * double(aenv_.coef)); }
    void setLfo3Rate(float r) { lfo3Rate_ = r; }
    // FUN_10010c50: the element's mod row (modVersion & 0xff) changed: copy it into this voice (last = 0)
    void syncModRow();  // FUN_1000ef10: tempo-synced LFO3 of playing voices
    int lfo3Sync() const { return e_ ? e_->lfo3.sync : 0; }
    // Part render end tests (FUN_100139c0): 1) +0x534 != 0 and amp env level < 1e-4; 2) a sample one-shot that has
    // ended (render modes 2 / >= 0x10) and whose filter has decayed.
    bool ampEnded() const { return mayEnd_ != 0 && aenv_.level < 1e-4f; }
    bool oneShotEnded() const { return oscFinished(); }
    void deactivate() { active_ = false; }  // removed from the part's voice list (FUN_10010130)
    int endState() const { return mayEnd_; }  // +0x534: 0 sounding, 1 may end (released / faded), 2 stolen
    float ampLevel() const { return aenv_.level; }  // +0x29c
    float velWord() const { return vel_; }          // +0x2f4
    virtual bool percLevel(float& /*out*/) const { return false; }  // Drawbar voices: the percussion level
    int stamp = 0;    // +0x528: part note counter (+0xb7c) at note-on / steal (oldest-voice choice)
    int unison = 0;   // +0x52c: unison stack index (param_7)
    void render(float* outLR, int frames);  // adds into interleaved stereo (note delay, then renderOsc)
    void endCheck();  // part render (FUN_100139c0) voice-end tests, after every render call
    // The original has one voice object per slot for every type; the port re-creates it when the type changes.
    // This carries over the words the next note-on reads before writing them (rate words, amp, last pitch).
    void inheritSlot(const Voice& old);

protected:
    // ---- type hooks (defaults = Sample element) ----
    // The type's case of the note-on switch (FUN_10014910 @100152ce), after the shared setup (and, for Sample only,
    // the note-on matrix pass); `startMod` = the Wave destination's value from that pass (the sample start-point
    // offset). May scale levelMod_ / add to panMod_ (v[0x98] / v[0x99]) before the pan law. Must leave the voice
    // ready to render.
    virtual void startOsc(const VoiceStart& vs, float startMod, MsvcRand& rng);
    // Per control block, after the matrix: the oscillator increment from pitch_ (default: min(exp(pitch + rateLn),
    // maxInc), FUN_1004ab60 @1004adda).
    virtual void oscControl();
    // FUN_1004ab60 @1004ae1b: the filter coefficients from F = exp(lastPitch * CutoffKey + cutoff) (default: the
    // shared SVF + one-pole on f_).
    virtual void filterControl(double F);
    // Note-on Click: X = the click level (the gain ramps are already set); Analog also seeds its filter states.
    virtual void onClick(double /*X*/) {}
    // Per-sample loop for `frames` frames: oscillator, filter, gain ramps; adds into out. Must call controlUpdate()
    // whenever --blockCount_ < 0 (every 32 samples), as the sample loop does.
    virtual void renderOsc(float* out, int frames);
    // Extra voice-end test (sample: a one-shot that has ended and whose filter has decayed).
    virtual bool oscFinished() const;
    // The shared control update (FUN_1004ab60); virtual so a type can append its per-block code.
    virtual void controlUpdate();
    // After FUN_10049eb0 at render entry (the element was set again): types that keep a pointer to their element data
    // re-read it (the original reads the element words live).
    virtual void typeRefresh() {}
    // Render entry before the note-delay test, when the element's version differs from the voice's (+0x520): the
    // drawbar render (FUN_10051470) re-copies the drawbar weights here (FUN_1004a030); other types do nothing.
    virtual void renderEntry() {}

    struct ModRt {  // runtime row (voice+0x318 + 0x20*i)
        const float* src = nullptr;
        float w1 = 0, w2 = 0;
        const float* mod = nullptr;
        float last = 0;
        float* dest = nullptr;
        float state = 0;
        int special = 0;
    };

    void runMatrix();
    void processSlot(ModRt& m);
    void nextStage(ExpEnv& env, bool isAmp, int fromStage);  // fromStage -1 = legato attack (+0x870 / +0x874)
    // Note-on Click in a legato retrigger (FUN_10016820, Analog only): adds X to the type's gain ramps.
    virtual void legatoClick(double /*X*/) {}
    void env1Stage(int stage);
    // FUN_1004a340 fVar7 = float((part+0x954 - 1) * Tempo to Env Rates + 1): stage times scale with 120 / bpm
    double tempoFactor() const {
        return double(float((double(in_->tempoFactor) - 1.0) * double(e_->tempoToRates) + 1.0));
    }
    void refresh();  // FUN_10049eb0
    const float* sourcePtr(int idx);
    float* destPtr(int idx);

    const ElementParams* e_ = nullptr;
    const PatchInputs* in_ = nullptr;
    MsvcRand* rng_ = nullptr;
    EngineConsts c_;
    const SampleData* s_ = nullptr;
    const Zone* z_ = nullptr;
    bool active_ = false, released_ = false;
    int mayEnd_ = 0;  // FIX: voice+0x534, set by the amp env stages (FUN_1004a340): 1 may end, 2 stolen
    float noteGain_ = 1.f;  // +0x530 (key/velocity crossfade gain, amp env peak factor)
    int lfoRead_ = 0;       // +0x2e4: next pair of PatchInputs::lfoArr
    bool waitingForNoteOff_ = false;  // Note Delay = "Note Off": v[1] = -1 until note-off
    int64_t heldSamples_ = 0;
    int delay_ = 0;
    float glideTarget_ = 0, glideCoef_ = 1;  // v[0x94], v[0x92] (glide_ = v[0x91] moves toward the target)
    float* lastGlide_ = nullptr;
    int note_ = 0;
    float bpm_ = 120.f;
    int paramVersion_ = 0;  // voice+0x520: the element version FUN_10049eb0 last ran for (0 in a fresh voice)

    // playback
    int ch_ = 1;
    bool quietSkip_ = false;  // Sample render mode 0x14 (FUN_1004bb10): no filter / output while the gains are 0
    int64_t pos_ = 0, end_ = 0, loopLen_ = 0;
    int step_ = 1;
    float phase_ = 0, inc_ = 0, maxInc_ = 16.f;
    float rateLn_ = 0, loopPitchDelta_ = 0;  // v[0x87] = +0x21c, v[0x86] = +0x218 (maxInc_)
    float y0_[2] = {0, 0}, slope_[2] = {0, 0};
    int blockCount_ = 0;

    // fixed sources
    static constexpr float zero_ = 0.f, one_ = 1.f;
    float rand_ = 0, vel_ = 0, notePitch_ = 0, lastPitch_ = 0;
    float sus_ = 0, soft_ = 0;              // +0x304 / +0x308 (mod sources 12 / 13)
    float scale2_ = 0;                      // +0x310 (mod source 20): 0.01 * tuning 2 cents of the note
    int relKey_ = 0;                        // note byte of the releasing note-off
    uint32_t modVersion_ = 0;               // +0x518
    const float* pitchSrc_ = nullptr;       // v[0x93]: PatchInputs::pitchOfs or ::fineTune
    // destinations / accumulators (voice words 0x11.. etc.). The note-on does not reset them: the rate words
    // (v[0xb2], v[0x9e]) and amp (v[0x2a]) keep the previous note's values (0 in a fresh voice).
    float pitch_ = 0, wave_ = 0, osc1Pitch_ = 0, cutoff_ = 0, k_ = 0, amp_ = 0, pan_ = 0;
    float lfo3RateMod_ = 0, env1RateMod_ = 0, dummy_ = 0;
    // note-on offsets v[0x95..0x99] (wave, reso, cutoff, level factor, pan) and the bases FUN_10049eb0 derives
    float waveMod_ = 0, resoMod_ = 0, cutoffMod_ = 0, levelMod_ = 1, panMod_ = 0;
    float glide_ = 0, pitchBase_ = 0, waveBase_ = 0, resoBase_ = 0, cutoffBase_ = 0, levelBase_ = 0, panBase_ = 0;
    float osc1PitchBase_ = 0;  // v[0x90]: Analog's osc 2 pulse width base (+0x5e0), 0 for the other types

    // modulators
    Lfo lfo3_;
    float lfo3Rate_ = 0, lfo3Val_ = 0, lfo1Val_ = 0, lfo2Val_ = 0;
    float env1Level_ = 0, env1Slope_ = 0, env1Time_ = 0, env1Len_ = 0;
    int env1Stage_ = 0;
    ExpEnv fenv_, aenv_;
    ModRt mods_[16];
    int modCount_ = 0;

    // filter (per channel: lp, bp, hp, xprev, u, t)
    struct Svf {
        float lp = 0, bp = 0, hp = 0, xprev = 0, u = 0, t = 0;
    } f_[2];
    float a_ = 0, b_ = 0, cc_ = 0, kk_ = 0;
    float resoLimit_ = 0.77f, c2Scale_ = 0.63f;
    bool freqFlag_ = true, invert_ = false;
    int tap_ = 0;
    float energy_ = 0;

    float gainL_ = 0, incL_ = 0, gainR_ = 0, incR_ = 0;
};

}  // namespace hs2
