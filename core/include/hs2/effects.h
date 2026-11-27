#pragma once
// Hypersonic 2 effects. Type numbers are the factory indices (FUN_10043e20): an Effect element / send FX record
// stores its type in param 0x13 as trunc(v*128.9) and its 12 parameters in params 0x19..0x24 (setParam 0..11).
// Effect vtable: +4 setSampleRate, +0x10 process(stereo interleaved, in place), +0x14 setParam. See
// docs/engine/effects_table.json and ghidra/decomp/effects.c.
#include <memory>
#include <vector>

namespace hs2 {

struct MsvcRand;

class Effect {
public:
    virtual ~Effect() = default;
    virtual void setSampleRate(float sr) = 0;
    virtual void setParam(int index, float value) = 0;  // index 0..11, value 0..1
    virtual void process(float* lr, int frames) = 0;
    // vtable +0xc: called by the element with (part block +0x88, +0x8c), tempo-related. Not called by the oracle,
    // so effects must behave like the original when it is never called.
    virtual void setTempo(float, float) {}
    // vtable +8: the element connects the part's note pitch (part+0x968: newest held note * ln2/12 + bend, -10 when
    // no note is held) before setSampleRate, whenever part+0x964 < 0.5 (normally always). Used by 7 effects:
    // 7 Tuned Resonators, 14 Ducking Delay, 15 Grain Delay, 55/56 Rotary Speakers, 70 Vintage Filter, 103 Amp Sim.
    // Test with fx_compare --pitch. The pointer must stay valid for the effect's lifetime.
    virtual void setPitchSource(const float*) {}
    // The original's CRT rand() is one generator shared by the voice engine and the effects that use it
    // (13 Tape Delay, 15 Grain Delay, 70 Vintage Filter), so the call order interleaves. The part passes its own
    // generator here; standalone (fxtest / fx_oracle) each effect keeps a private one seeded like srand(1).
    virtual void setRandom(MsvcRand*) {}
    // process() arguments 3/4 in the original: the song position (quarter notes, double) of the part render call
    // (FUN_100191b0 param_3; the host ppq at the block start, advanced by frames * beats per sample). Read by the sync
    // modes of 31 Step Flanger, 71 Wah Pedal and 72 TalkBox. The part sets it before every process call.
    void setSongPos(double ppq) { songPos_ = ppq; }

    // Part render protocol (FUN_100139c0): `silent` is true while nothing upstream has made sound in this block.
    // Default = the common pattern in the original process methods: on signal, reload the tail countdown (+8 from
    // +0xc); while silent, skip processing once the countdown is below zero, otherwise count down, process and clear
    // `silent`. Effects with another pattern override this (see fx_compare --gated).
    virtual void processGated(float* lr, int frames, bool& silent) {
        if (!silent) {
            gateCount_ = gateLen_;
        } else {
            if (gateCount_ < 0) return;
            gateCount_ -= frames;
            silent = false;
        }
        process(lr, frames);
    }

protected:
    double songPos_ = 0.0;    // see setSongPos (0 = what fx_oracle passes)
    int gateCount_ = 0;       // effect +8 (base ctor FUN_10025400: 0)
    int gateLen_ = 200000;    // effect +0xc: tail length in samples (base default 200000; some setSampleRates set it)
};

// nullptr for type 128 (no effect: group bus) and for types not ported yet.
std::unique_ptr<Effect> createEffect(int type);
const char* effectName(int type);  // "(none)" for 128, "?" for a number that is no effect type
// True for an effect type the port has (0..127, the ones createEffect makes); not for 128 (no effect).
bool effectTypeExists(int type);

}  // namespace hs2
