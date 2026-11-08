#pragma once
// hs2::Engine: the original's 16-part multitimbral instrument (the synth object, FUN_1007c690, and the plugin layer
// that drives it). One part per MIDI channel (each an hs2::PatchPlayer in engine mode), a global part that holds the
// four global send FX, the master gain / tone enhancer, the plugin's VST parameter set (part mixer and MIDI settings,
// links, global FX) and combi loading. See docs/NOTES.md "Engine container, combis".
//
//   Engine e(content, sr);               // = plugin ctor + synth ctor + resume(): 16 empty parts
//   e.loadCombi(Combi::parse(chunk));    // setChunk FUN_1008c270 (or e.loadPatch(part, patch))
//   per host block: e.processBlock(outLR, frames, events, numEvents, &transport);
//
// Events carry their MIDI channel in the status byte. processBlock = processEvents FUN_1008d5e0 (routing by channel,
// part links, velocity / touch tables, bank select, RPN / NRPN, program-change requests) + process FUN_1008d400
// (host sync FUN_100884c0, the multi-part render FUN_1007bcf0: each channel's part split at its events, parts still
// sounding after a patch change, the global send FX, the overflow guard, the tone enhancer on used outputs).
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hs2/combi.h"
#include "hs2/content.h"
#include "hs2/dsp.h"
#include "hs2/patch.h"
#include "hs2/synth.h"

namespace hs2 {

class Engine {
public:
    static constexpr int kParts = 16;
    static constexpr int kParamsPerPart = 0x98;  // VST parameters: part p's parameter j = p * 0x98 + j (j >= 1)
    static constexpr int kGlobalFxBase = 0x980;  // global send FX k's parameter j = 0x980 + k * 0x18 + j
    static constexpr int kNumParams = 0x9e0;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    // outputs = stereo output pairs (the original's Outputs setting); part / FX outputs above it clamp to it.
    Engine(Content& content, float sampleRate, int outputs = 1);
    ~Engine();

    // ---- loading (the plugin's loaders; the new part is used from the next processBlock, the old one keeps
    // sounding until it is silent, as the original does) ----
    // FUN_10089ef0(blob, part, 0, bank): Engine::loadPatch + the part settings the plugin keeps (Mute, Level unless
    // it is below 0.01, Output; with "Keep" (part parameter 0x19) also pan, sends, ranges, ...; the others come from
    // the patch). bank: the patch's bank number for the default scene root rule (patches of banks 0..99), -1 = none.
    void loadPatch(int part, const Patch& patch, int bank = -1);
    void clearPart(int part);  // FUN_10089c90 (a part that holds a patch becomes empty)
    // setChunk FUN_1008c270 with a combi: parts, global send FX, master volume, links (+ setup values when stored).
    void loadCombi(const Combi& combi);

    // ---- per host block ----
    void processBlock(float* outLR, int frames, const MidiEvent* events, int numEvents,
                      const HostTransport* transport);
    // Multi-output form: outs[p] = interleaved stereo pair p (adds into it), p < outputs().
    void processBlockMulti(float* const* outs, int frames, const MidiEvent* events, int numEvents,
                           const HostTransport* transport);
    void resume();  // the plugin's resume(): the next block's host tempo counts as a change

    // ---- parameters ----
    // The plugin's setParameter (FUN_1008a950): 0 = Master Volume; part p (0-based), j = 1..0x97: index p*0x98 + j
    // (j = 0 of parts 1..15: Link to the part below); global send FX k: 0x980 + k*0x18 + j. Part j: 1 Mute, 2 Level,
    // 3 Pan, 4 Output, 5..8 FX Send 1..4, 9..14 Hyperknobs, 15..20 Key lo / hi / fade, Vel lo / hi / fade,
    // 21 Transpose, 22 Fine Tune, 23 Polyphony, 24 FX Mode, 25 Keep, 26 Level Trim, 27 / 28 PB Up / Down, 29 Mono,
    // 30 Glide, 31 Glide Time, 32..38 microtuning, 39..47 arpeggiator, 56 + s*24 + j': patch send FX s.
    void setParameter(int index, float value);
    float getParameter(int index) const { return index >= 0 && index < kNumParams ? vst_[size_t(index)] : 0.f; }
    void setPartParameter(int part, int j, float v) { setParameter(part * kParamsPerPart + j, v); }
    // FUN_100121a0 on a part (elem 0 = patch, 1..0x40 element records, 0x41..0x44 send FX) or, part = -1, the global
    // part (elem 0x41..0x44 = the global send FX).
    void setPartSetter(int part, int elem, int idx, float value);
    float getPartSetter(int part, int elem, int idx) const;

    // Setup values (FUN_10088ab0 defaults; combis with settings version > 0x3f / 0x4f store them).
    void setMemoryMode(float v);       // FUN_10089340: patch param 1 of every part; 0 ECO, 0.5 MID (default), 1 XXL
    void setHype(float v);             // FUN_10089380: tone enhancer drive (0.33 default) and the master gain
    void setVelSens(float v);          // FUN_10083ea0 (0.5 = linear, default)
    void setTouchSens(float v);        // FUN_10083fb0 (0.5 = linear, default)
    void setMasterTune(int semis, float fine);  // FUN_10088720
    void setGmMode(bool on) { gm_ = on; }
    // Not in the original: effect tails ring out instead of being cut when a part falls silent (PatchPlayer::setRingOut).
    void setRingOut(bool on);
    bool ringOut() const { return ringOut_; }
    // Not in the original engine: a load (loadPatch, loadCombi, clearPart) stops the part's previous patch with a
    // 10 ms fade when the new one starts, instead of letting its notes and effect tails sound on. Default off.
    void setCutOnLoad(bool on) { cutOnLoad_ = on; }
    bool cutOnLoad() const { return cutOnLoad_; }
    void setMidiCombiSelect(int mode) { combiSel_ = mode; }  // 0 off, 1 on (default), 2 chain
    // The Combi Chain (128 x bank * 256 + program, -1 = empty; parseCombiChain reads an .h2c file). With MIDI Combi
    // Select = 2 a program change on any channel requests chain entry `program` instead of a patch.
    void setCombiChain(const std::vector<int32_t>& chain);

    // FUN_1000ef10 for every part via FUN_1007baf0 (tests: one sync after loading, as hs2render without --host).
    void syncTempo(double bpm, double bps, double bpb, double ppq, double barStart, int flags);

    // Program changes seen by processBlock (FUN_1008d5e0): the original queues them for its loader thread
    // (bank * 256 + program, with bank select CC0 * 128 + CC32 per channel; on channel 16 banks 120..128 are combi
    // banks). The host loads them: a patch with loadPatch, a combi with loadCombi.
    struct ProgramRequest { int channel; int bank; int program; };
    std::vector<ProgramRequest> takeProgramRequests();

    int outputs() const { return outputs_; }
    PatchPlayer& part(int p) { return *slots_[size_t(p)].latest(); }
    PatchPlayer& globalPart() { return *global_; }
    float masterGain() const { return master_; }
    const std::string& combiName() const { return combiName_; }

private:
    struct Slot {
        std::unique_ptr<PatchPlayer> cur;               // current part (synth+0xc8 node)
        std::vector<std::unique_ptr<PatchPlayer>> old; // still sounding after a patch change (newest first)
        std::vector<std::unique_ptr<PatchPlayer>> pending;  // loaded, used from the next render (oldest first)
        PatchPlayer* latest() const { return pending.empty() ? cur.get() : pending.back().get(); }
        bool dropOld = false;                           // cutOnLoad: fade out the old parts once pending is empty
        struct Fading { std::unique_ptr<PatchPlayer> p; int left, len; };
        std::vector<Fading> fading;                     // cutOnLoad: parts fading out (frames left of len)
    };
    struct Ev { int offset; uint8_t m[4]; };

    std::unique_ptr<PatchPlayer> newPart();
    void loadPatchEngine(int ch, const Patch* patch);          // FUN_1007bbb0
    void loadIntoPart(int ch, const Patch* patch, bool restore, int bank);  // FUN_10089ef0
    void setMaster();                                            // FUN_10089380 with the stored hype
    void addEvent(int ch, bool linked, int ofs, uint8_t s, uint8_t d1, uint8_t d2, uint8_t d3);  // FUN_100886c0
    void processEvents(const MidiEvent* events, int n, int frames);  // FUN_1008d5e0
    void nrpn(int ch, int param, float v);                           // FUN_10087fb0
    double hostSync(const HostTransport* t);                         // FUN_100884c0
    void render(int frames, double pos);                             // FUN_1007bcf0
    void renderFading(Slot& s, int frames, double pos);              // cutOnLoad (not in the original)
    void enhance(float* x, int n, int ch);                           // FUN_1007bcf0 tone enhancer, one channel
    void queueSoundOff(int ch) { ring_.push_back(ch); }              // FUN_100886a0(ch << 24 | 0x17800)

    Content& content_;
    float sr_;
    int outputs_;
    MsvcRand rng_;                     // the CRT rand(): one generator for every part and effect
    std::array<Slot, kParts> slots_;
    std::unique_ptr<PatchPlayer> global_;  // synth+4: the global send FX
    PartBus bus_;
    std::vector<float> outBuf_, gbusBuf_;
    std::array<std::vector<Ev>, kParts> lists_;  // per-channel event lists (plugin +0xb588)
    std::vector<int> ring_;            // queued all-sound-off events (FUN_100886a0 ring)
    std::vector<ProgramRequest> requests_;

    // plugin state
    std::array<float, kNumParams> vst_{};    // +0xb54
    bool link_[kParts] = {};                  // +0xb608 + 4k: part k + 1 plays the events of part k
    uint8_t velTab_[128] = {}, touchTab_[128] = {};
    int bankSel_[kParts] = {};                // +0xb688: CC0 * 128 + CC32
    int rpn_[kParts] = {};                    // +0xb648: RPN / NRPN number
    int bankProg_[kParts] = {};               // +0x32e0: the last requested bank * 256 + program
    float memory_ = 0.5f, hype_ = 0.33f, velSens_ = 0.5f, touchSens_ = 0.5f;
    int tuneSemis_ = 0;
    float tuneFine_ = 0.f;
    bool gm_ = false;
    bool ringOut_ = false, cutOnLoad_ = false;
    std::vector<float> fadeBuf_;
    int combiSel_ = 1;
    std::array<int32_t, 128> chain_;
    int sceneRoot_ = 0x18, sceneMidi_ = 0;
    std::string combiName_;
    bool firstBlock_ = true;                  // +0xb6cc

    // synth state
    float master_ = 0;                        // content+0x12dc
    float masterTune_ = 0;                    // content+0x12e0
    int enhMode_ = 0;                         // synth+0x14c
    float enhG_ = 0, enhA_ = 0;               // +0x150, +0x154
    float enhState_[32][4] = {};              // synth+0x158: per output channel
    bool overflowLogged_ = false;             // synth+0x358
    // tempo: synth+0x128.. (stored sync for new parts) and the plugin's host sync words
    double sBpm_ = 120.0, sBps_ = 0, sBpb_ = 4.0, sBar_ = 0;
    int sFlags_ = 0x60;
    double hSpb_ = 0, hBpm_ = 0, hBps_ = 0, hBpb_ = 4.0, hPos_ = 0, hBar_ = 0;
    bool hPlaying_ = false;
};

}  // namespace hs2
