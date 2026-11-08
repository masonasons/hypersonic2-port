#pragma once
// Arpeggiator / phrase sequencer ("CHyperArp"), a 0x468-byte object at part+0xc44, functions 0x10045510..0x10048840.
// Bit-exact port, verified against the original in the emulator (tools/emu/arp_oracle.py, tools/cli/arptest.cpp,
// tools/emu/arp_compare.py: identical event streams, identical object state after every call, identical rand()).
//
// The state is the original object byte for byte (ArpState, offsets checked by static_assert), so the code can index
// it exactly as the original does (a few computed indices land in neighbouring fields; the port does the same).
// Differences from the original object:
//  * +0x3f0 (step-record table pointer, always the object itself: the part never passes another one) is a 0/1 word.
//  * +0x3f4 / +0x3f8 (current / pending phrase pointers) are 0/1 words; the data lives in phrase buffers owned by the
//    Arp (copies of the phrase bank entry, from +0x1f0: float length in beats, int event count, then 0x18-byte events
//    {double time, double length, u8 status, data1, data2, data3, pad[4]}). The buffers are zero-padded, so the
//    out-of-range event reads the original can make (mode switched to Phrase while running with a step index beyond
//    the phrase) read zeros instead of heap garbage.
//  * Divisions by a zero note count and a null phrase in modes 10..15 (both crash the original, and cannot happen
//    through the part's normal paths) return without doing anything.
//
// Part wiring: the comments on each method name the calling address in the part.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "hs2/dsp.h"

namespace hs2 {

// The original object, field by field (names are descriptive; the offset is the identity).
struct ArpState {
    struct Step {               // 16 step records, patch params 0xac + 5*step + field (written by the part's setter)
        float note;             // +0x00  field 0 (Step Seq: semitones = trunc(note*48+0.5) - 24; Elem Seq: trunc(note*16+0.5) + 35)
        float vel;              // +0x04  field 1, stored as 2v
        float gate;             // +0x08  field 2
        int32_t seq1;           // +0x0c  field 3, trunc(v*126.9) -> CC 0x80 byte
        int32_t seq2;           // +0x10  field 4, trunc(v*126.9) -> CC 0x81 byte
    };
    Step steps[16];             // +0x000
    float params[17];           // +0x140  raw values of setParam(i, v)
    uint8_t on;                 // +0x184  On
    uint8_t latch;              // +0x185  Latch
    uint8_t running;            // +0x186
    uint8_t releasePending;     // +0x187  compact the note list at the next render call (latch off / pedal up)
    uint8_t sustain;            // +0x188  CC 64 >= 64 while the arp is on
    uint8_t stopRequest;        // +0x189  stop at the next render call
    uint8_t pad18a[2];
    int32_t mode;               // +0x18c  trunc(v*15.9): 0 Up 1 Down 2 Up+Down 3 As Played 4 Random 5 Step Seq
                                //         6 Rand Seq 7 Ctrl Seq 8 Elem Seq 9 Phrase (10..15 reachable, no generator)
    int32_t octaves;            // +0x190  trunc(v*3.9) + 1
    int32_t numSteps;           // +0x194  trunc(v*16.9) (0 = no wrap)
    int32_t syncMode;           // +0x198  trunc(v*7.9)
    int32_t transpose;          // +0x19c  trunc(v*48.9) - 24
    int32_t splitKey;           // +0x1a0  trunc(v*128.9) (Elem Seq: keys below it are not taken by the arp)
    int32_t hiKey;              // +0x1a4  trunc(v*128.9) (stored only)
    int32_t scaleKey;           // +0x1a8  trunc(v*12.9): 0 -> 12 (off), else n - 1 (black keys are moved up)
    float stepLen;              // +0x1ac  Rate table value (beats)
    float gateLen;              // +0x1b0  stepLen * Gate + 0.001
    float swing;                // +0x1b4
    // Held-note list (object at +0x1b8: FUN_10046700 add, FUN_10045ee0 remove, FUN_10045fd0 compact)
    int32_t noteCount;          // +0x1b8
    uint8_t noteDirty;          // +0x1bc  entries flagged inactive (latched / sustained releases)
    uint8_t played[16][3];      // +0x1bd  in played order: note, velocity, active
    uint8_t sorted[16][3];      // +0x1ed  sorted by note
    uint8_t pad21d[3];
    // Pending note-offs (32-entry ring, kept sorted by time)
    uint8_t offNote[32];        // +0x220
    double offTime[32];         // +0x240  (beats)
    int32_t offCount;           // +0x340
    int32_t offHead;            // +0x344
    // Output MIDI events (32-entry ring of 4-byte events)
    uint8_t out[32][4];         // +0x348
    int32_t outCount;           // +0x3c8
    int32_t outHead;            // +0x3cc
    float sampleRate;           // +0x3d0
    uint8_t pad3d4[4];
    double songPos;             // +0x3d8  beats
    double samplesPerBeat;      // +0x3e0
    double beatsPerSample;      // +0x3e8
    uint32_t stepTable;         // +0x3f0  (pointer to the step records; here 1 = own records)
    uint32_t phrase;            // +0x3f4  (pointer to the current phrase; here 1 = phrase loaded)
    uint32_t pendingPhrase;     // +0x3f8  (pointer to a phrase taken at the next render call; here 1 = pending)
    uint8_t pad3fc[4];
    double nextTime;            // +0x400  time of the next arp action (beats)
    int32_t stepIndex;          // +0x408
    uint8_t nextIsNoteOff;      // +0x40c  0: the next action is a step, 1: a pending note-off
    uint8_t lastNote[16];       // +0x40d  note of the previous step per chord voice (0xff = none)
    uint8_t pad41d[3];
    double stepTime;            // +0x420  start of the current step
    double phraseStart;         // +0x428  phrase origin (beats)
    float swingPending;         // +0x430  swing change applied at the next odd step (-1 = none)
    uint8_t pad434[4];
    double swungTime;           // +0x438  re-timed next step after a rate change (-1 = none)
    uint8_t phraseFlush;        // +0x440  release all pending notes before the next phrase note
    uint8_t playing;            // +0x441  host transport playing (sync flags bit 0)
    uint8_t phraseDrum;         // +0x442  first phrase event is 0x99 (stored only)
    uint8_t phraseThru;         // +0x443  first phrase event has byte 3 == 1 (bank 10): MIDI passes to the part
    int32_t w444;               // +0x444  set to -1 by setSampleRate, not read
    int32_t chordIndex;         // +0x448  phrase chord-note pointer (commands 0x24..0x3a)
    int32_t octaveShift;        // +0x44c  phrase octave (commands 0x36/0x37), -1..1
    int8_t pattern[4];          // +0x450  current pattern (FUN_10045bd0)
    int8_t patPos[3];           // +0x454  pattern positions (kinds 0/1, 2, 3)
    uint8_t patActive;          // +0x457
    int32_t patKind;            // +0x458
    int32_t extraTranspose;     // +0x45c  phrase command 0x38: a second note at +n semitones (-24..24)
    int32_t bounceDown;         // +0x460  phrase command 0x3a direction
    float phraseScale;          // +0x464  1 / (0.5 / stepLen): phrase beats per Rate step
};
static_assert(sizeof(ArpState) == 0x468);
static_assert(offsetof(ArpState, params) == 0x140 && offsetof(ArpState, on) == 0x184);
static_assert(offsetof(ArpState, mode) == 0x18c && offsetof(ArpState, stepLen) == 0x1ac);
static_assert(offsetof(ArpState, noteCount) == 0x1b8 && offsetof(ArpState, played) == 0x1bd);
static_assert(offsetof(ArpState, sorted) == 0x1ed && offsetof(ArpState, offNote) == 0x220);
static_assert(offsetof(ArpState, offTime) == 0x240 && offsetof(ArpState, offCount) == 0x340);
static_assert(offsetof(ArpState, out) == 0x348 && offsetof(ArpState, outCount) == 0x3c8);
static_assert(offsetof(ArpState, sampleRate) == 0x3d0 && offsetof(ArpState, songPos) == 0x3d8);
static_assert(offsetof(ArpState, stepTable) == 0x3f0 && offsetof(ArpState, nextTime) == 0x400);
static_assert(offsetof(ArpState, lastNote) == 0x40d && offsetof(ArpState, stepTime) == 0x420);
static_assert(offsetof(ArpState, swungTime) == 0x438 && offsetof(ArpState, phraseFlush) == 0x440);
static_assert(offsetof(ArpState, chordIndex) == 0x448 && offsetof(ArpState, pattern) == 0x450);
static_assert(offsetof(ArpState, patActive) == 0x457 && offsetof(ArpState, phraseScale) == 0x464);

class Arp {
public:
    // The original's CRT rand() is shared with the voice engine: pass the part's generator.
    explicit Arp(MsvcRand& rnd);
    Arp(const Arp&) = delete;
    Arp& operator=(const Arp&) = delete;

    // FUN_100467b0: constructor body (part ctor FUN_10018b80 @10018f28). Defaults: 44100 Hz, 120 bpm timing words.
    void init();
    // FUN_10046340 (part setSampleRate FUN_100137f0 @100139a9): re-applies the 17 stored params, stops.
    void setSampleRate(float sr);
    // FUN_100460b0(i, v), i = patch param index - 0x11 (0x11..0x21 except 0x1a/0x1b; part setter FUN_100121a0
    // @10013435 with the value already clamped to 0..1 by the setter).
    void setParam(int i, float v);
    // Part setter FUN_100121a0 cases 0xac..0xfb (@10013565..1001362b): step record (idx-0xac)/5, field (idx-0xac)%5.
    void setStep(int patchIdx, float v);
    // Phrase objects. A phrase bank entry (slot 1, after any SyncrosoftContentHeader) and a patch's embedded phrase
    // slot have the same layout: u32 size, ..., +0x28 flags, +0x2c six Hyperknob values, +0x5c patch params 0x11..,
    // +0xac 0x50 step values, +0x1f0 the phrase. The part keeps up to 64 slot objects (part+0xc48[i]); FUN_10016e20(i)
    // makes slot i current and hands its +0x1f0 to the arp with FUN_10045510(0, obj + 0x1f0) (for i == 0 always; for
    // i != 0 only when its event count is non-zero, after applying the slot's knobs / params / steps).
    //
    // FUN_10045510(0, obj + 0x1f0): the pending phrase, taken by the next render call (next() -> FUN_10045540).
    // Copies the object (allocates: not for the audio thread). An object shorter than 0x1f8 bytes gives an empty phrase.
    void setPhraseObject(std::span<const uint8_t> obj);
    // FUN_1001afb0(6, bank, prog): entry `prog` of the phrase bank "NNN ..." in <contentRoot>/Phrases, as the
    // content manager's reader returns it, plus the setter's bank-10 fix (@100134d0: in bank 10 the first event's byte 3
    // is set to 1 when the phrase has events, which makes it a "thru" phrase). Empty when the bank or entry is missing
    // or empty (the original then changes nothing).
    static std::vector<uint8_t> loadPhraseEntry(const std::filesystem::path& contentRoot, int bank, int prog);
    // Part setter case 0x1b (@10013445) as a whole for slot 0: bank = trunc(p[0x1a]*128.9), prog = trunc(p[0x1b]*128.9);
    // nothing when bank + prog == 0 or the entry is empty (returns false); else loadPhraseEntry + setPhraseObject.
    bool setPhrase(const std::filesystem::path& contentRoot, int bank, int prog);
    // The patch blob's trailer (patch load FUN_10016f70 @100174dd: the dword after the last element / send-FX record).
    // kind 0 (legacy): after the params the load sets patch params 0x95 = 0, 0x96 = 0, 0x97 = 1, 0x1e = 1 through the
    // setter. kind 2: a 0x100-byte MIDI CC map (copied to part+0x1a0), int lastSlot, then for slot 0..lastSlot a u32
    // size and an object of that size (the size word is the object's first field; size 0 = empty slot, 4 bytes); each
    // object goes to part slot i (0..63), and slot 0 (== part+0xd48, 0 after a load) through FUN_10016e20(0); then the
    // setter applies 0x95 again. 61 factory patches (84/1.., 86/0..49, all Phrase mode) embed their phrase this way.
    struct PatchTrailer {
        uint32_t kind = 0;
        int lastSlot = -1;
        std::vector<std::span<const uint8_t>> slots;  // empty span = empty slot
    };
    static PatchTrailer patchTrailer(std::span<const uint8_t> blob);
    // FUN_10045710(flags, bpm, ppq), from the part's host sync FUN_1000ef10 @1000efb4. Flags: bit 0 playing,
    // 2 stop (sets the stop request), 4 start / 8 jump (moves all arp times by ppq - songPos), else just songPos = ppq.
    void sync(int flags, double bpm, double ppq);
    // FUN_10046980(msg): the part's MIDI handler FUN_10017880 @100178cb calls it for every host event (not for
    // events from the arp); true = consumed (the part returns at once). Status bytes are compared whole (channel 0).
    bool midiIn(const uint8_t msg[4]);
    // FUN_100485d0(frames, songPos, ev): frames to render before the next arp action, or `frames`. Writes ev (4
    // bytes) only when it pops an output event; it never clears it. The part's render wrapper FUN_100191b0 keeps one
    // ev per render call, zeroed once at the start, and sends it whenever ev[0] != 0 after a call that returned
    // r < frames (so a stale event is sent again when a call pops nothing: an original quirk the part must keep).
    // `wrote` (optional) reports whether this call wrote ev.
    int next(int frames, double songPos, uint8_t ev[4], bool* wrote = nullptr);

    // Words the part reads.
    bool on() const { return s_.on != 0; }               // +0x184
    bool running() const { return s_.running != 0; }     // +0x186
    int mode() const { return s_.mode; }                 // +0x18c
    const ArpState& state() const { return s_; }
    // The object bytes as the original holds them, pointer words as 0/1 (for state comparisons).
    void stateBytes(uint8_t out[sizeof(ArpState)]) const;

private:
    // Original helpers (thiscall on the arp unless noted)
    void applyPhrase(int stepTab, bool hasData);                         // FUN_10045540
    void pushEvent(int b0, int b1, int b2, int b3);                      // FUN_100457e0
    void addNoteOff(int note, double t);                                 // FUN_10045880
    bool isNewNote(int note, int last) const;                            // FUN_100458e0
    void extendNoteOff(int note, float add);                             // FUN_10045950
    int blackKey(int note) const;                                        // FUN_100459d0
    static int phraseVel(int evVel, int noteVel, bool raw);             // FUN_10045a30
    static int stepVel(float scale, int vel);                            // FUN_10045a90
    double stepGate(float g2, float gate) const;                         // FUN_10045b00
    bool chance(bool on, int noteVel, int evVel);                        // FUN_10045b30 (rand)
    void loadPattern(int kind, int count);                               // FUN_10045bd0
    void retime(float newLen);                                           // FUN_10045c30
    void sortNoteOffs();                                                 // FUN_10045d50
    double phraseSwing(double t) const;                                  // FUN_10045ea0
    void noteListRemove(uint8_t note, bool keep);                        // FUN_10045ee0 (on +0x1b8)
    void noteListCompact();                                              // FUN_10045fd0 (on +0x1b8)
    double nextActionTime(double prev);                                  // FUN_100463a0
    void phraseNote(int ev, int note, bool scale, int vel);              // FUN_10046550
    void noteListAdd(uint8_t note, uint8_t vel);                         // FUN_10046700 (on +0x1b8)
    void step();                                                         // FUN_10046da0
    void phraseStep(int count);                                          // FUN_10046da0 mode 9 (@1004769f)
    void arpTail(int note, int vel);                                     // FUN_10046da0 modes 0..4 shared tail
    void stepSeq(int rec, bool randomBug, int ri);                       // FUN_10046da0 modes 5/6
    void patternUpdate(int count);                                       // FUN_10046980 mode 9 pattern refresh

    uint8_t* raw() { return reinterpret_cast<uint8_t*>(&s_); }
    const uint8_t* raw() const { return reinterpret_cast<const uint8_t*>(&s_); }
    // A byte at a computed object offset (as the original indexes it); out-of-object offsets read 0 / are ignored.
    uint8_t byteAt(int off) const { return off >= 0 && off < int(sizeof(ArpState)) ? raw()[off] : 0; }
    void setByte(int off, uint8_t v) {
        if (off >= 0 && off < int(sizeof(ArpState))) raw()[off] = v;
    }
    // Phrase data (the original's +0x3f4 pointer target): length @0, count @4, event i @8 + 0x18 i.
    float phraseLen() const;
    int32_t phraseCount() const;
    void setPhraseCount(int32_t n);
    double evTime(int i) const;
    double evLength(int i) const;
    uint8_t evByte(int i, int b) const;  // b = 0 status, 1 data1, 2 data2, 3 data3

    ArpState s_{};
    MsvcRand* rnd_;
    std::vector<uint8_t> phraseBuf_, pendingBuf_;  // from entry + 0x1f0, zero-padded
};

}  // namespace hs2
