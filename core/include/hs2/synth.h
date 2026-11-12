#pragma once
// Plays one patch: elements (sample layers + insert effects, in patch order on one bus) -> voices, plus the patch's
// send effects. Port of the part: MIDI FUN_10017880, voice list / allocation FUN_100102b0 / FUN_10010130, render
// FUN_100191b0 / FUN_100139c0 / FUN_1000f130, tempo sync FUN_1000ef10, and the plugin-level host sync
// (FUN_100884c0 / FUN_1007baf0) and main render (FUN_1007bcf0) for one part.
//
// Plugin use (see docs/NOTES.md "PatchPlayer API"):
//   PatchPlayer p(content, sr); p.load(patch); p.resume();
//   per host block: p.processBlock(out, frames, events, numEvents, &transport);
// processBlock splits the block at the events exactly as the original does; the output depends on the host's block
// sizes the same way the original's does (the patch LFOs tick once per 32 frames of a part render call).
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hs2/arp.h"
#include "hs2/content.h"
#include "hs2/effects.h"
#include "hs2/patch.h"
#include "hs2/voice.h"

namespace hs2 {

// One MIDI event of a host block (VstMidiEvent: deltaFrames + midiData). The channel nibble is ignored.
struct MidiEvent {
    int offset = 0;         // frame inside the block, 0 <= offset < frames (events at or after `frames` are dropped)
    uint8_t data[4] = {};   // status, data1, data2, data3 (note detune in cents for note-ons; 0 from a host)
};

// The host's transport at the start of a block, as VstTimeInfo gives it (getTimeInfo(0x2e00) in FUN_100884c0).
struct HostTransport {
    double bpm = 120.0;          bool tempoValid = true;   // kVstTempoValid
    double ppq = 0.0;            bool ppqValid = true;     // kVstPpqPosValid: position in quarter notes
    double barStart = 0.0;       bool barValid = true;     // kVstBarsValid: last bar start in quarter notes
    int sigNum = 4, sigDen = 4;  bool sigValid = true;     // kVstTimeSigValid
    bool playing = false;                                  // kVstTransportPlaying
};

class Engine;

// The content manager's shared buses and the engine outputs as a part inside hs2::Engine sees them (FUN_100139c0 /
// FUN_1000f130): the global send FX buses 1..4 (content buses, one block long, cleared by the global part after
// use) with their "used" flags (part+0x1138 [1..4]), and the output pairs (interleaved stereo, one block long) with
// the per-channel "used" flags (content+0x7c) that gate the tone enhancer.
struct PartBus {
    int outputs = 1;                 // stereo outputs (part+0xbac / element +0x840)
    float* out[16] = {};             // output pair p (0-based)
    bool outUsed[32] = {};           // per output channel
    float* gbus[4] = {};             // global send FX bus k (0-based slot)
    bool gused[4] = {};
};

class PatchPlayer {
public:
    PatchPlayer(const PatchPlayer&) = delete;
    PatchPlayer& operator=(const PatchPlayer&) = delete;
    // sharedRand: the generator to use instead of the player's own (hs2::Engine passes one for every part: the
    // original's CRT rand() is one generator for all parts and effects).
    PatchPlayer(Content& content, float sampleRate, MsvcRand* sharedRand = nullptr);

    // Engine::loadPatch (FUN_1007bbb0): a new part, synced to the last host tempo (FUN_1000ef10 with the synth's
    // stored tempo, before the patch's parameters are set), then the patch load.
    void load(const Patch& patch);
    // The plugin's resume() (FUN_10088b.. "resume()"): clears the host-sync tempo word, so the next block with a
    // valid host tempo counts as a tempo change and syncs the part (flags 0x20).
    void resume();

    // One host block: host sync (FUN_100884c0 -> FUN_1007baf0 -> FUN_1000ef10 when the tempo, time signature or
    // transport changed), then FUN_1007bcf0 for this part: part render calls split at the events (each a full
    // FUN_100191b0 with the song position advanced by frames * beats per sample), the global send FX once for the
    // whole block, and the master tone enhancer. Adds into interleaved stereo `out`. Events must be sorted by
    // offset. transport may be nullptr (no host time info: no sync, the song position keeps counting).
    void processBlock(float* outLR, int frames, const MidiEvent* events, int numEvents,
                      const HostTransport* transport);
    // processBlock without events or transport (kept for simple callers and tests).
    void render(float* outLR, int frames) { processBlock(outLR, frames, nullptr, 0, nullptr); }

    // Immediate MIDI input (FUN_10017880 with flag 0): note on/off, CC (incl. sustain, legato, expression, mod wheel,
    // all notes off, reset, mono/poly and the CC->parameter map), pitch bend, channel pressure.
    void midi(uint8_t status, uint8_t d1, uint8_t d2, uint8_t d3 = 0) {
        uint8_t m[4] = {status, d1, d2, d3};
        midiEvent(m, false);
    }
    void noteOn(int note, int velocity) { midi(0x90, uint8_t(note), uint8_t(velocity)); }
    void noteOff(int note) { midi(0x80, uint8_t(note), 0); }
    // FUN_100121a0 (part setParam): elem 0 = patch parameter idx, elem >= 1 = parameter idx of element record elem-1.
    // Ported for the cases MIDI controllers reach (the CC map) and the element records; see NOTES.md.
    void setParam(int elem, int idx, float value);
    // FUN_1000ef10 directly (tests / the oracle's --tempo): bpm, beats per sample, beats per bar, song position and
    // bar start in quarter notes, flags (bit 0 playing, 2 stop, 4 start, 8 jump, 0x20 tempo, 0x48/0x50 signature).
    void syncTempo(double bpm, double bps, double bpb, double ppq, double barStart, int flags);

    const std::vector<std::string>& warnings() const { return warnings_; }

    // Engine::loadPatch's third argument (part+0x14, the plugin's memory mode / piano quality setting): a patch whose
    // stored Piano Mode (0x54) is above 0.99 gets this value instead (0 ECO, 0.5 MID, 1 XXL). Applies to the next load.
    void setMemoryMode(float v) { memoryMode_ = v; }

    // Ring-out mode: NOT in the original (default off = the original's behaviour). The original puts a part to sleep
    // after its first silent call, and its delay/reverb gate (FUN_10025480) only looks at the left channel, so a delay
    // echo still on its way (or only on the right) is cut off when the last voice ends (e.g. 039/2 "Soloing"). With
    // ring-out an insert or send effect counts as audible for kRingSeconds after its input or output (either channel)
    // was last audible, so the part stays awake and its tail plays out.
    void setRingOut(bool on) { ringOut_ = on; }
    static constexpr double kRingSeconds = 4.5;

    // Decodes every sample the loaded patch's sample elements can play (the zone set a note would use), so the first
    // notes do not decode on the audio thread. For a loader thread; returns the decoded bytes now held. Samples are
    // shared and kept by the content (SampleBank cache), so a part loaded later gets them at once.
    // keepGoing (optional) is asked before each sample; false stops the preload there (a newer load, a memory budget).
    size_t preloadSamples(const std::function<bool()>& keepGoing = {});

    // Every decoded sample this part can still read: the zones of its sample elements (both zone sets) and the
    // samples its voices hold. Whatever no part lists may be released (Content::releaseSamples) while no player runs.
    struct SampleRefs {
        std::vector<std::pair<const SampleBank*, uint32_t>> zones;
        std::vector<const SampleData*> voices;
    };
    void collectSampleRefs(SampleRefs& out) const;

    // Voices playing now (list entries, part+0xe08; not in the original's API).
    int activeVoices() const { return count_; }

    // Part getParam FUN_1000cea0: elem 0 = patch parameter, 1..0x40 element record elem-1, 0x41..0x44 send FX slot.
    float getParam(int elem, int idx) const;
    const Patch& patch() const { return patch_; }
    static const ParamArray& defaultParams();  // a new part's patch parameters (part ctor FUN_10018b80)

private:
    friend class Engine;
    struct Elem {
        ElementParams p;
        float quality3 = 0;                   // patch param 1 (part quality) at the element's last setter case 3
        int orig = 0;                         // index in the patch (element 0 is never processed as an insert)
        bool isEffect = false;
        bool muted = false;                   // effect: +0x41c (keeps its Aux Out routing)
        int fxType = 128;                     // +0xba4
        bool playable = false;                // sample layer with a valid instrument, or a supported other type
        SampleBank* bank = nullptr;
        const Instrument* inst = nullptr;     // +0xbb4 (nullptr: no such instrument, e.g. a piano layer switched off)
        int wave = 0;                         // +0xbb0 low byte: the instrument index after the piano layer selection
        std::unique_ptr<Effect> fx;           // insert (nullptr = group bus / not ported)
        int auxOut = 0;                       // +0x838 trunc(v*15.9): 0 none, >1 = output here and clear the bus
        float outGain = 0, pan = 0.5f;        // 4 v^2 (2 * elem+0x5b8), elem+0x5d8
        float send[4] = {};                   // v^2 * 4
        uint32_t noteCounter = 0;             // +0xb6c: notes that passed the key range (trigger 7/8, alt pan)
        float lastGlide = 0;                  // +0x4f8: previous note's glide target
        ParamArray raw;                       // stored params after the loader fixups (runtime setParam source)
        float knobApplied[12] = {};           // effect: the knob values last passed to fx->setParam
        int ring = 0;                         // ring-out countdown (frames; not in the original)
        std::vector<std::shared_ptr<const TypeParams>> retired;  // replaced typeParams playing voices may point to
        // Element setter state that decides how many setter calls bump the version word +0x5ec (verCall): the type
        // (+0x424), Receive CC (+0x428, 1 in a new element) and the quick-knob destinations (+0xcc0.., 0x10 = OFF).
        int simType = 0;
        bool simRcc = true;
        int simDest[4] = {0x10, 0x10, 0x10, 0x10};
    };
    struct SendFx {
        std::unique_ptr<Effect> fx;
        bool routed = false;                  // +0x83c != 0
        float level = 0, pan = 0.5f, send1 = 0;  // 4*raw Output Level (applied as 4*level), raw pan, send to FX1
        int quick[2] = {0, 0};                // quick-knob sources (0x91 / 0x92: trunc(v*7.9))
        ParamArray raw{};
        // engine mode (hs2::Engine; the element setter FUN_100221f0 on slots 0x41..0x44):
        int auxOut = 0;                       // +0x83c: min(trunc(v*32.9), outputs), 0 = no return
        bool muted = false;                   // +0x41c (Mute; effects are not muted by their level)
        int fxType = 128;                     // +0xba4 (a new effect object only when the type changes)
        int ring = 0;                         // ring-out countdown (frames; not in the original)
    };
    // Voice list entry (part+0xe0c + 8 i): the 64 slots are a permutation; the first count_ entries are playing,
    // sorted by element; the next free slot is the one at position count_ (the most recently freed).
    struct Entry {
        int8_t slot = 0, elem = 0, note = 0, held = 0;  // held: the note while held, 0 released, -1 sustained
        int32_t group = 0;                               // +4: the element's Mute Condition
    };

    void midiEvent(uint8_t* m, bool fromArp);       // FUN_10017880(event, flag)
    void noteOnEvent(int note, int vel, int detune, bool fromArp);
    void noteOffEvent(uint8_t noteByte);
    void controller(int cc, int value);
    void pitchBend(int lsb, int msb);
    // FUN_100191b0 / FUN_100139c0 + FUN_1000f130. Return true when the part was silent (FUN_100139c0's result).
    bool partRender(float* out, int frames, double songPos, int sendOfs);
    bool renderCall(float* out, int frames, double songPos, int sendOfs);
    double hostSync(const HostTransport* t);                               // FUN_100884c0 (+ FUN_1007baf0)
    void processInsert(int ei, bool beforeLastVoice, int frames, bool& fxSilent, bool& partSilent, float* out,
                       float* const* sendBus, bool* const* sendUsed, int ofs);
    // Engine mode: FUN_1000f130 for the part's own send FX (ofs = frame offset in the block) or, for the global
    // part, ofs < 0: the global send FX on the content's global buses. Returns true when an effect made sound.
    bool sendFxRender(int frames, double songPos, int ofs, bool* ownUsed);
    void ringOut(int& ring, bool inSilent, bool& outSilent, const float* lr, int frames) const;
    void setSendFxParam(int slot, int idx, float value);  // FUN_100221f0 on a send FX element (0x41 + slot)
    void loadSendFxEngine(const Patch& patch);  // FUN_10016f70's send FX records (own slot or the global part)
    static SendFx freshSendFx();                // a new element's send FX slot (element ctor FUN_10025050)
    PatchPlayer* globalPart_ = nullptr;         // part+0x4bc (engine mode; null for the global part itself)
    void addOut(int pair, int ofs, const float* src, int frames, float gl, float gr);  // engine output pair (1-based)
    int elementGain(int ei, int tnote, int vel, uint32_t trig, bool monoHasVoices, const Zone*& zone,
                    const SampleData*& sd);
    void computePianoSelect();            // patch setter cases 0x48 / 0x4b / 0x54: part+0x974
    // Microtuning (patch 0x4c..0x53): tuning k (0: part+0xbf4, 1: part+0xbf8) = a Tunings bank entry (int32 count,
    // count floats in cents), loaded by FUN_10010770 when its bank / entry index changes.
    struct Tuning {
        bool loaded = false;               // the handle is non-null and resident
        int32_t count = 0;
        std::vector<float> cents;
    };
    void loadTuning(int k);
    double tuneCents(int note, float* scale2) const;  // FUN_10010870
    void bindSample(Elem& el);            // element setter cases 0x7e / 0x7f (Sample Bank / Wave, piano layers)
    int pianoZoneVel(const Elem& el, int vel) const;  // FUN_1001c0f0: zone velocity of piano layers 1 / 5
    int allocate(int ei);                 // FUN_100102b0: list position for a new voice of element ei
    void removeEntry(int pos);            // FUN_10010130
    void stealEntry(int pos);             // FUN_100101a0
    void muteCondition(int mode, int ei, int note);  // FUN_10010670
    int pickMono(int ei, int mode) const;            // FUN_10010200
    void startVoice(int note, int cents, int vel, float gain, int ei, int pos, int unison, const Zone* zone,
                    const SampleData* sd);           // FUN_10014910 (part side)
    void legato(int pos, int note, int cents, int vel, int mode);  // FUN_10016820
    void setFxKnob(int type1, int type2, int knob, float value);  // FUN_10010d20
    void mainOut(float* out, int frames);  // FUN_1000fa30: bus * part gains with the output ramp
    void updateOutputGain();              // part+0xb8c / +0xb90 (patch setter cases 0x3b / 0x43 / 0x3c)
    void computePatchLfos();              // patch setter cases 0x55..0x5c
    void setKnob(int k, float value);     // patch setter cases 0x0e / 0x0f / 0x67 / 0x69 / 0x6b / 0x6d
    void macro(int slot);                 // FUN_1000ca70: re-applies the macro slot to the Receive CC/EE elements
    void reapplyElement(int ei, int times = 1);  // element setter call(s): recompute ElementParams, version += times
    // FUN_100221f0(idx, v) as far as the version word goes: how many +0x5ec bumps the call makes (with the setter's own
    // nested calls: type change, Receive CC, quick-knob destinations), updating the Elem's sim state.
    int verCall(Elem& el, int idx, float v);
    int macroParam(int slot, const Elem& el) const;  // FUN_1000ca70: the element param a macro slot re-sets
    void elementKnobsToFx(int ei);        // also refreshes effectRouting
    void effectRouting(Elem& el);         // insert Aux Out / level / pan / sends from the knob-routed params        // effect element: route changed knobs to fx->setParam
    float partKeyVelGain(int note, int vel) const {
        return float(int(partVelGain_[vel & 127]) * int(partKeyGain_[note & 127]));
    }

    Content& content_;
    EngineConsts c_;
    PatchInputs inputs_;
    Patch patch_;                         // the loaded patch with runtime parameter changes (part+0x4d0 params)
    Lfo lfo1_, lfo2_;
    LfoParams lfo1P_, lfo2P_;
    std::vector<float> lfoArr_;           // part+0xdd4
    std::vector<Elem> elems_;
    std::vector<int> elemIndex_;          // patch record index -> elems_ index (-1 = skipped)
    // Voice slots; each holds the voice class of the element type it last played (created on demand).
    // TODO(realtime): preallocate per type instead of allocating at note-on.
    std::array<std::unique_ptr<Voice>, 64> voices_;
    ElementType voiceType_[64] = {};
    std::array<Entry, 64> list_;
    int count_ = 0;                       // part+0xe08
    int poly_ = 32;                       // part+0xbb0: patch Max Voices
    bool asleep_ = false;                 // part+0x1140
    bool ringOut_ = false;                // see setRingOut (not in the original)
    MsvcRand ownRng_;
    MsvcRand& rng_;                       // ownRng_, or the engine's generator shared by every part
    // Engine mode (hs2::Engine): the shared buses / outputs, the master gain (content+0x12dc), master tune
    // (content+0x12e0), the part's Output (+0xba8, patch 0x3d: min(trunc(v*32.9), outputs), 0 = off).
    PartBus* eng_ = nullptr;
    float master_ = 0;                    // set in the ctor to the standalone constant
    float masterTune_ = 0;
    int partOutput_ = 1;
    float* engOut_[16] = {};              // engine output pairs at the current render call's frame offset
    std::unique_ptr<Arp> arp_;            // part+0xc44 (CHyperArp), shares rng_
    std::vector<uint8_t> phraseSlot0_;    // part+0xc48[0]: the current phrase object
    std::array<std::vector<uint8_t>, 64> phraseSlots_;  // part+0xc48[1..63] (index 0 unused: phraseSlot0_)
    void selectScene(int i);              // FUN_10016e20
    std::vector<std::string> warnings_;
    float outL_ = 1.f, outR_ = 1.f;  // patch gain * patch pan law * master gain
    float rampL_ = 0.f, rampR_ = 0.f;  // part+0xb84 / +0xb88: the output ramp's current gains
    bool rampReset_ = true;            // part+0x18: set while asleep; the next call starts the ramp at the gains
    float patchSend_[4] = {};
    bool ownSends_ = false;
    SendFx sendFx_[4];
    std::vector<float> bus_, sendBus_[4], globalBus_[4];
    bool globalUsed_[4] = {};
    // Master tone enhancer (main render FUN_1007bcf0, set up by FUN_1007c610): y = x + g*(x - x[n-k] + lp2[n-k])
    struct Enhancer {
        float s1 = 0, s2 = 0, carry[2] = {0, 0};
        int phase = 0;
    } enh_[2];
    float enhG_ = 0, enhA_ = 0;
    int enhMode_ = 0;
    std::vector<float> mix_;
    // Part note pitch (part+0x968), connected to effects through Effect::setPitchSource: (highest held note +
    // transpose) * ln2/12 + pitch offset (+0xbd4), -10 when no note is held and the sustain pedal is up.
    // Effects keep a pointer to it, so PatchPlayer must not move.
    float notePitch_ = -10.f;
    uint32_t partNotes_ = 0;  // part+0xb7c: note-on / steal counter (trigger flag bit 0 = parity, voice stamps)
    // MIDI controller words (FUN_10017880)
    float modWheelRaw_ = 0;   // +0xda0 (trigger conditions 1/2; copied to +0xda4 = inputs_.modWheel per render call)
    float cc1Copy_ = 0;       // +0xdc8
    float atRaw_ = 0;         // +0xda8 (smoothed into +0xdac = inputs_.aftertouch per render call)
    float softPedal_ = 0;     // +0xdbc (CC67 >> 6)
    int sustain_ = 0;         // +0xd98 (CC64 >> 6; trigger conditions 3/4, notes held as -1)
    int legatoPedal_ = 0;     // +0xd9c (CC68 >> 6)
    float bendSemis_ = 0;     // +0xbd0
    int pbUp_ = 0, pbDown_ = 0;  // +0xbb8 / +0xbbc patch PB Range / PB Down trunc(v*15.9) (13, 14, 15 special)
    int redamper_ = 0;        // +0xbf0 patch Redamper trunc(v*127.9)
    int patchType_ = 5;       // +0x4b4 patch Patch Type
    bool partMute_ = false;   // +0xba4 patch Mute: note-ons start nothing
    float pianoChar_ = 0;     // +0x970 patch Piano Character - 0.375 (Seq1 source in patch type 2)
    int pianoSel_ = 0;        // trunc(part+0x974): Piano Mode * 6 + trunc(Piano Reso * 2.9) + (Piano Model 2 ? 3 : 0)
    float memoryMode_ = 0.5f; // part+0x14 (Engine::loadPatch argument)
    Tuning tuning_[2];
    int tuneBank_[2] = {0, 0}, tuneEntry_[2] = {0x80, 0x80};  // part+0xbd8 / +0xbdc, +0xbe4 / +0xbe8 (ctor: 0, 0x80)
    int tuneRoot_[2] = {0, 0};  // part+0xbe0 / +0xbec: trunc(v * 24 + 0.5) - 12
    float tuneDepth_ = 0;       // part+0xbfc: |v - 0.5| (v - 0.5) * 16 (tuning 2 only)
    float seq1_ = 0;          // +0xdc0 (arp Seq1, internal CC 0x80)
    int maxVel_ = 0;          // +0x112c
    int64_t dbgClock_ = 0;    // frames rendered (HS2_ARPLOG debug output only)
    bool modReset_ = false;   // +0x978 = 1.0 after a mod row's source / mod / dest changed
    uint32_t resumeFrom_ = 0; // +0xb80: Envelope Resume only retriggers voices stamped from here on
    std::array<int16_t, 128> partKeyGain_{}, partVelGain_{};  // +0x97c / +0xa7c patch key / velocity crossfades
    int8_t noteDetune_[128] = {};  // +0x120: byte 3 of the last host note-on per note (cents)
    int16_t ccMap_[128] = {};      // +0x1a0: CC -> (elem << 8 | param), -1 = none (FUN_1000c830)
    // tempo (part words set by FUN_1000ef10)
    double bpmD_ = 120.0, bpsD_ = 120.0 / 60.0 / 44100.0;  // +0xd90 / +0xd88
    float bpm_ = 120.f, bps_ = 0;  // +0x958 / +0x95c
    // host sync (plugin words) and the synth's stored sync (synth+0x128..0x148, used by loadPatch)
    double hBpm_ = 0, hBps_ = 0, hBpb_ = 4.0, hPos_ = 0, hBar_ = 0, hSpb_ = 0;
    bool hPlaying_ = false;
    double sBpm_ = 120.0, sBps_ = 0, sBpb_ = 4.0, sBar_ = 0;
    int sFlags_ = 0x60;
    // held notes: part+0x1c count, part+0x20 in note-on order, part+0xa0 sorted by pitch
    int heldCount_ = 0;
    int8_t heldOrder_[130] = {}, heldSorted_[130] = {};
    int transpose_ = 0;  // FIX: part+0xbb4 = trunc(p[0x37]*72 + 0.5) - 36 (patch setter case 0x37)
};

}  // namespace hs2
