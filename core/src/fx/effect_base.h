#pragma once
// Shared base for effect ports (original base class ctor FUN_10025400): 12 stored params, defaults p0 = p2 = p3 = 0.5.
// Each effect lives in core/src/fx/fx_NNN_*.cpp and is verified against the emulated original with
// tools/emu/fx_compare.py (see docs/NOTES.md "Effects").
//
// Porting rules (every effect so far reached bit-exact, -240 dB, by following them):
//  * The original runs the x87 at 53-bit precision (MSVC default control word 0x027F). Values it keeps on the FPU
//    stack are plain double; values it stores to float memory are rounded at exactly those points. Follow the
//    disassembly (tools/emu/disasm.py), not Ghidra: the decompile hides most store points. An `fst` stores a float
//    copy but keeps using the unrounded register value.
//  * Integers derived from float maths (buffer lengths, tap positions) follow from the above
//    (e.g. int(double(sr) * double(1.51f)), not int(sr * 1.51f)). Float->int conversions truncate.
//  * exp: the original's fldl2e/frndint/f2xm1/fscale sequence; see x87exp in fx_020_chorus.cpp. std::sin/cos match.
//  * Ghidra's decompile can drop writes through pointers; when the oracle disagrees, trust the oracle.
//  * Vtable +8 connects the part's note pitch (Effect::setPitchSource); test such effects with fx_compare --pitch.
//  * Do not accept ~-80 dB as "x87 precision": it has always turned out to be a float rounding at the wrong place.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "hs2/effects.h"
#include "registry.h"

namespace hs2 {

class EffectBase : public Effect {
public:
    EffectBase() {
        std::memset(p_, 0, sizeof p_);
        p_[0] = p_[2] = p_[3] = 0.5f;
    }
    void setParam(int i, float v) override {
        if (i < 0 || i >= 12) return;
        p_[i] = v;
        onParam(i, v);
    }

    // Silence gating (part render protocol, see Effect::processGated). Standard is Effect's default; Detect is the
    // reverb/delay pattern: the countdown branch leaves `silent` set, and after processing a still-audible output
    // (FUN_10025480: any left sample with 2|x| > 1e-5) reloads the countdown and clears it.
    void processGated(float* lr, int frames, bool& silent) override {
        if (gate_ != Gate::Detect) {
            Effect::processGated(lr, frames, silent);
            return;
        }
        if (!silent) {
            gateCount_ = gateLen_;
        } else {
            if (gateCount_ < 0) return;
            gateCount_ -= frames;
        }
        process(lr, frames);
        if (silent && hasSignal(lr, frames)) {
            gateCount_ = gateLen_;
            silent = false;
        }
    }

protected:
    enum class Gate { Standard, Detect };
    Gate gate_ = Gate::Standard;
    static bool hasSignal(const float* lr, int frames) {  // FUN_10025480 (left channel only)
        for (int i = 0; i < frames; ++i) {
            const float a = std::fabs(lr[2 * i]);
            if (double(a) + double(a) > double(1e-05f)) return true;
        }
        return false;
    }
    virtual void onParam(int, float) {}
    void reapplyParams() {
        for (int i = 0; i < 12; ++i) onParam(i, p_[i]);
    }
    float p_[12];
    float sr_ = 44100.f, invSr_ = 1.f / 44100.f;
};

}  // namespace hs2
