#include "hs2/synth.h"
#include "hs2/element_types.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace hs2 {

namespace {
constexpr float kDrive = 0.33f;  // plugin default (this+0xae8); TODO: expose as the setup parameter
constexpr float kBias = 1e-6f;   // FIX: FUN_100139c0 / FUN_1000f130 add 1e-6 to frame 0 of a bus (0x358637bd)

void addScaled(float* dst, const float* src, int frames, float gl, float gr) {  // FUN_1000ffe0 / FUN_1000f660
    for (int i = 0; i < frames; ++i) {
        dst[2 * i] += src[2 * i] * gl;
        dst[2 * i + 1] += src[2 * i + 1] * gr;
    }
}
// FUN_1000f660 as the x87 runs it (53-bit: dst = float(g * x + dst)); engine mode, where several parts add into one
// buffer (the standalone player keeps its float version, so its renders stay unchanged).
void addScaledX87(float* dst, const float* src, int frames, float gl, float gr) {
    for (int i = 0; i < frames; ++i) {
        dst[2 * i] = float(double(gl) * double(src[2 * i]) + double(dst[2 * i]));
        dst[2 * i + 1] = float(double(gr) * double(src[2 * i + 1]) + double(dst[2 * i + 1]));
    }
}

// FUN_1000c830 (patch setter case 0x44, Patch Type): the part's CC -> parameter map (part+0x1a0, a short per CC:
// element << 8 | parameter, -1 = none). FUN_10017880 sends a mapped CC to the setter FUN_100121a0 instead of its
// own controller cases.
void buildCcMap(int16_t* m, int type) {
    for (int i = 0; i < 128; ++i) m[i] = -1;
    struct Map { int cc, target; };
    static constexpr Map kDefault[] = {
        {5, 0x94},  {7, 0x43},  {9, 0x00},  {10, 0x3c}, {16, 0x0e}, {17, 0x0f}, {18, 0x67}, {19, 0x69},
        {65, 0x03}, {69, 0x11}, {70, 0x7e}, {71, 0x81}, {72, 0x87}, {73, 0x85}, {74, 0x80}, {75, 0x86},
        {76, 0x7f}, {77, 0x83}, {78, 0x84}, {79, 0x82}, {80, 0x6b}, {81, 0x6d}, {91, 0x3f}, {92, 0x40},
        {93, 0x41}, {94, 0x42}};
    for (const auto& x : kDefault) m[x.cc] = int16_t(x.target);
    switch (type) {
        case 1: {  // drum kits: element record 0 parameters
            static constexpr int kT[11] = {0x115, 0x162, 0x121, 0x166, 0x11d, 0x11e, 0x11f, 0x16a, 0x168, 0x123, 0x18b};
            for (int i = 0; i < 11; ++i) m[20 + i] = int16_t(kT[i]);
            break;
        }
        case 2: m[70] = 0x47; break;  // piano: Piano Character
        case 3: {                     // organ: drawbar element parameters
            for (int i = 0; i < 9; ++i) m[12 + i] = int16_t(0x119 + i);
            m[31] = 0x219;
            m[66] = 0x115;
            m[70] = 0x116;
            m[71] = 0x117;
            m[72] = 0x118;
            break;
        }
        case 4: m[77] = 0x1b0; m[78] = 0x1ae; break;
        default: break;
    }
}

// Hyperknob parameters: value, ofs, rng, assignment (patch setter cases 0x0e.., FUN_1000cc40)
struct KnobDef { int value, ofs, rng, assign; };
constexpr KnobDef kKnobs[6] = {{0x0e, 0x71, 0x72, 0x70}, {0x0f, 0x74, 0x75, 0x73}, {0x67, 0x77, 0x78, 0x76},
                               {0x69, 0x8b, 0x8c, 0x8a}, {0x6b, 0x8e, 0x8f, 0x8d}, {0x6d, 0x7c, 0x7d, 0x7b}};
}  // namespace

PatchPlayer::PatchPlayer(Content& content, float sampleRate, MsvcRand* sharedRand)
    : content_(content), c_(EngineConsts::make(sampleRate)), rng_(sharedRand ? *sharedRand : ownRng_) {
    c_.content = &content_;
    master_ = 0.5f * 0.5f * (8.f - 2.2f * kDrive) * 0.25f;
    if (kDrive > 0.01f) {
        enhG_ = kDrive * 2.2f;
        enhA_ = 700.f / sampleRate;
        enhMode_ = sampleRate >= 64000.f ? 2 : 1;
    }
    for (int i = 0; i < 64; ++i) list_[i].slot = int8_t(i);  // part ctor: the identity permutation
    lfoArr_.assign(64, 0.f);
    inputs_.lfoArr = lfoArr_.data();
    // plugin resume(): +0xb00 = 1 / (sr * 60); synth ctor: 120 bpm, 4 beats per bar
    hSpb_ = 1.0 / (double(sampleRate) * 60.0);
    hBps_ = 120.0 * hSpb_;
    sBps_ = 120.0 * hSpb_;
    bpsD_ = sBps_;
    bps_ = float(bpsD_);
    buildCcMap(ccMap_, 5);
    partKeyGain_.fill(10000);
    partVelGain_.fill(10000);
    arp_ = std::make_unique<Arp>(rng_);
}

void PatchPlayer::resume() { hBpm_ = 0.0; }

void PatchPlayer::updateOutputGain() {
    // Part output (FUN_100139c0): gain = master * Level^2 * Trim^2 * 8 (patch params 0x43, 0x3b),
    // pan (0x3c): x = (p - p^2) * 0.83, L = (1 - p + x) g, R = (p + x) g.
    // Master (FUN_10089380 / FUN_1007c610): b54^2 * (8 - 2.2 * drive) / 4 with defaults b54 = 0.5, drive = 0.33.
    // TODO: VST master/channel volume parameters.
    const auto& pp = patch_.params;
    const double lvl = pp[0x43], trim = pp[0x3b], pan = pp[0x3c];
    const float master = master_;  // content+0x12dc (standalone: the constant above; engine: FUN_10089380)
    // setter (@1001225c): +0xb8c = float(level * level * trim * trim * 8); render (@10013a6a): G = master * +0xb8c,
    // x = (p - p^2) * 0.83, L = float((1 - p + x) * G), R = float((x + p) * G), all in the x87 registers
    const float b8c = float(lvl * lvl * trim * trim * 8.0);
    const double G = double(master) * double(b8c);
    const double x = (pan - pan * pan) * double(0.83f);
    outL_ = float(((1.0 - pan) + x) * G);
    outR_ = float((x + pan) * G);
}

// Patch LFO1/LFO2 words (patch setter cases 0x55..0x5c): the free rate (0xdd8 / 0xddc), the tempo factor (0xdec /
// 0xdf4) and the note value (0xde8 / 0xdf0). The rate becomes bpm * tempo factor only at the next host sync
// (FUN_1000ef10).
void PatchPlayer::computePatchLfos() {
    const auto& pp = patch_.params;
    const auto macros = computeMacros(pp);
    const auto weights = computeMacroWeights(pp);
    const int rateCtl = quant(pp[0x46], 7.9f);  // case 0x46: part+0xbc4
    auto lfoP = [&](int base, int bit) {
        LfoParams l;
        l.wave = quant(pp[base], 15.9f);
        l.sync = quant(pp[base + 1], 4.9f);
        float r = pp[base + 2];
        // FIX: cases 0x57 / 0x5b: with part+0xbc4 bit 0 / 1 the LFO Rate macro (Hyperknob slot 1, part+0x910) is added,
        // with the slot's weight (part+0x8d4). The Hyperknob setters re-apply the rate through FUN_1000ca70 once the
        // assignments are known, so the loaded patch ends up with the macro.
        if (rateCtl & bit) r = applyMacro(r, macros[1], weights[1]);
        l.rate = float(expF(float(double(r) * double(6.8024f) - double(2.70805f))) * double(c_.blockT));
        l.phase = pp[base + 3];
        // patch setter 0x57/0x5b: +0xde8 = noteValue, +0xdec = noteValue * blockT / 30
        l.noteVal = noteValue(r);
        l.tempoRate = float(double(l.noteVal) * c_.blockT * double(0.033333335f));
        return l;
    };
    lfo1P_ = lfoP(0x55, 1);
    lfo2P_ = lfoP(0x59, 2);
}

void PatchPlayer::load(const Patch& patch) {
    patch_ = patch;
    // FUN_10016f70 sets the patch parameters through the setter only for values in 0..1 (others keep the new part's
    // default; engine mode, where the plugin reads them back), and writes the old-patch Gate fixup (0x18 = 0.5 below
    // version 0xBEC874C) into the stored value too.
    if (eng_) {
        const ParamArray& def = defaultParams();
        for (int i = 0; i < 0x100; ++i) {
            const float v = patch_.params[size_t(i)];
            if (!(v >= 0.f && v <= 1.f)) patch_.params[size_t(i)] = def[size_t(i)];
        }
    }
    if (int32_t(patch.version) < 0xBEC874C && patch.params[0x18] >= 0.f && patch.params[0x18] <= 1.f)
        patch_.params[0x18] = 0.5f;
    // FUN_10016f70: a stored Piano Mode (0x54) above 0.99 is replaced by part+0x14, the memory mode Engine::loadPatch
    // (FUN_1007bbb0) stores in the new part before the load (the plugin's global setting; 0.5 = MID)
    if (patch_.params[0x54] > 0.99f && patch_.params[0x54] <= 1.f) patch_.params[0x54] = memoryMode_;
    if (patch.tailMarker == 0) {  // FUN_10016f70 @100174f0: old patches get setter (0x95, 0), (0x96, 0), (0x97, 1), (0x1e, 1)
        patch_.params[0x95] = 0.f;
        patch_.params[0x96] = 0.f;
        patch_.params[0x97] = 1.f;
        patch_.params[0x1e] = 1.f;
    }
    elems_.clear();
    elemIndex_.assign(patch.elements.size(), -1);
    warnings_.clear();
    // Engine::loadPatch builds a new part: empty voice list in slot order, nothing held, awake, controllers reset.
    for (auto& v : voices_) v.reset();
    for (int i = 0; i < 64; ++i) list_[i] = Entry{int8_t(i)};
    count_ = 0;
    heldCount_ = 0;
    partNotes_ = 0;
    asleep_ = false;
    rampReset_ = true;
    notePitch_ = -10.f;
    modWheelRaw_ = cc1Copy_ = atRaw_ = softPedal_ = bendSemis_ = seq1_ = 0.f;
    sustain_ = legatoPedal_ = maxVel_ = 0;
    modReset_ = false;
    resumeFrom_ = 0;
    std::memset(noteDetune_, 0, sizeof noteDetune_);
    inputs_ = PatchInputs{};
    std::fill(lfoArr_.begin(), lfoArr_.end(), 0.f);
    inputs_.lfoArr = lfoArr_.data();
    c_.expression = 1.f;
    // FUN_1007bbb0: the new part is synced to the synth's stored tempo (FUN_1000ef10) before the patch load; nothing
    // in the fresh part is tempo-synced yet, so only the tempo words change.
    lfo1P_ = lfo2P_ = LfoParams{};
    // part ctor: a new arp (FUN_100467b0); part setSampleRate FUN_100137f0 re-applies the new part's parameter
    // words (arp: 0x17 = 0.8, 0x18 = 0.5, 0x1c = 0.5, 0x1d = 0x3ed47ae1, 0x1e = 1, steps 0.5, others 0), then
    // the arp's setSampleRate (FUN_10046340)
    arp_->init();
    phraseSlot0_.clear();
    for (auto& s : phraseSlots_) s.clear();
    for (int i = 0x11; i <= 0x21; ++i) {
        if (i == 0x1a || i == 0x1b) continue;
        float d = 0.f;
        if (i == 0x17) d = 0.8f;
        else if (i == 0x18 || i == 0x1c) d = 0.5f;
        else if (i == 0x1d) d = std::bit_cast<float>(0x3ed47ae1u);
        else if (i == 0x1e) d = 1.f;
        arp_->setParam(i - 0x11, d);
    }
    for (int i = 0xac; i < 0xfc; ++i) arp_->setStep(i, 0.5f);
    arp_->setSampleRate(c_.sr);
    syncTempo(sBpm_, sBps_, sBpb_, sBar_, sBar_, sFlags_);

    const auto& pp = patch_.params;
    transpose_ = quant(pp[0x37] * 72.f + 0.5f, 1.f) - 36;
    poly_ = 1 + quant(pp[0x3a], 63.9f);  // patch setter case 0x3a: part+0xbb0
    updateOutputGain();
    // Patch sends (0x3e own send FX on/off, 0x3f..0x42 send levels v^2)
    ownSends_ = pp[0x3e] > 0.5f;
    for (int k = 0; k < 4; ++k) patchSend_[k] = pp[0x3f + k] * pp[0x3f + k];
    partMute_ = pp[0x00] > 0.5f;
    {  // Patch Type (case 0x44; 0 is stored as 5) -> CC map (FUN_1000c830)
        patchType_ = quant(pp[0x44], 15.9f);
        if (patchType_ == 0) patchType_ = 5;
        buildCcMap(ccMap_, patchType_);
        // the patch's own CC assignments (FUN_10016f70 after the records): marker 2 = a stored map; marker 0 = the
        // Hyperknob CCs in patch params 0x04, 0x05, 0x68, 0x6a, 0x6c, 0x6e (trunc(v*128.9) = CC + 1, 0 = none)
        if (patch.tailMarker == 2 && patch.ccMap.size() == 128) {
            for (int i = 0; i < 128; ++i) ccMap_[i] = patch.ccMap[i];
        } else if (patch.tailMarker == 0) {
            static constexpr int kSrc[6] = {0x04, 0x05, 0x68, 0x6a, 0x6c, 0x6e};
            static constexpr int kKnobParam[6] = {0x0e, 0x0f, 0x67, 0x69, 0x6b, 0x6d};
            for (int i = 0; i < 6; ++i) {
                const int cc = quant(pp[kSrc[i]], 128.9f);
                if (cc != 0) ccMap_[cc - 1] = int16_t(kKnobParam[i]);
            }
        }
    }
    buildXfade(partKeyGain_, pp[0x31], pp[0x32], pp[0x33]);  // cases 0x31..0x36: FUN_10048b80
    buildXfade(partVelGain_, pp[0x34], pp[0x35], pp[0x36]);
    pbUp_ = quant(pp[0x49], 15.9f);
    pbDown_ = quant(pp[0x4a], 15.9f);
    redamper_ = quant(pp[0x45], 127.9f);
    pianoChar_ = float(double(pp[0x47]) - double(0.375f));
    inputs_.seq1 = patchType_ == 2 ? pianoChar_ : seq1_;
    computePianoSelect();
    // Microtuning: a new part has bank 0 / entry 0x80 for both tunings; its setSampleRate applies the default patch
    // params (0x4c / 0x4d / 0x50 / 0x51 = 0, 0x53 = 0), which loads entry 0.0 for both; then the patch's values
    // (FUN_10010770 reloads when an index changes), so in effect the patch's bank / entry pair is loaded.
    for (int k = 0; k < 2; ++k) {
        const int b = quant(pp[k ? 0x50 : 0x4c], 128.9f), e = quant(pp[k ? 0x51 : 0x4d], 128.9f);
        tuneBank_[k] = b;
        tuneEntry_[k] = e;
        loadTuning(k);
        tuneRoot_[k] = int(double(pp[k ? 0x52 : 0x4e]) * 24.0 + 0.5) - 12;
    }
    {
        const double d = double(pp[0x53]) - 0.5;
        tuneDepth_ = float(std::fabs(d) * d * 16.0);
    }
    // case 0x38: +0xbc8 = ((2v - 1) + master tune (content+0x12e0; 0 standalone)) * ln2/12, +0xbd4 = bend * ln2/12
    // + that
    inputs_.fineTune = float(((double(pp[0x38] + pp[0x38]) - 1.0) + double(masterTune_)) * double(kSemitone));
    partOutput_ = std::min(quant(pp[0x3d], 32.9f), eng_ ? eng_->outputs : 1);  // case 0x3d: part+0xba8
    inputs_.pitchOfs = float(double(bendSemis_) * double(kSemitone) + double(inputs_.fineTune));

    // Patch LFO1/LFO2 (patch params 0x55..0x5c; setter FUN_100121a0, ticked in FUN_100139c0)
    computePatchLfos();
    // FIX: case 0x55 / 0x59 (the wave, set before the phase 0x58 / 0x5c) inits the LFO from the part's stored phase
    // when the wave changes. A new part's setSampleRate (FUN_100137f0) first applies the ctor defaults: LFO1 wave
    // 0.16 -> 2 (sine), LFO2 wave 0.28 -> 4 (saw: value 1), both with phase 0. A patch wave that differs inits again
    // from there (phase still 0), so e.g. S+H and drift waves start from the default wave's value word (0/65: LFO2
    // Slow Drift walks from 1).
    lfo1_ = Lfo{};
    lfo2_ = Lfo{};
    lfo1_.init(0.f, 2, rng_);
    lfo2_.init(0.f, 4, rng_);
    if (lfo1P_.wave != 2) lfo1_.init(0.f, lfo1P_.wave, rng_);
    if (lfo2P_.wave != 4) lfo2_.init(0.f, lfo2P_.wave, rng_);
    for (int i = 0; i < 6; ++i)  // Hyperknob values as mod sources (part+0xd58..: (value - ofs) * rng)
        inputs_.knob[i] = (pp[kKnobs[i].value] - pp[kKnobs[i].ofs]) * pp[kKnobs[i].rng];

    auto makeFx = [&](const ParamArray& p, const std::string& name, float* applied) {
        const int type = quant(p[0x13], 128.9f);
        auto fx = createEffect(type);
        if (fx) {
            fx->setRandom(&rng_);  // one CRT rand() shared with the voices
            // element setter case 0x6b: vtable +0xc tempo (part+0x958 bpm, +0x95c beats per sample; FUN_1000ef10),
            // then +8 note pitch (when part+0x964 < 0.5), then setSampleRate
            fx->setTempo(bpm_, bps_);
            fx->setPitchSource(&notePitch_);
            fx->setSampleRate(c_.sr);
            // FIX: quick-knob routes reach effect knobs. An insert element's load pass sees its own knob values
            // (0x0e / 0x0f); the loader's final Hyperknob calls then re-set the routed knobs (elementKnobsToFx).
            const ParamArray q = applied ? elementOwnKnobParams(p, pp) : elementKnobParams(p, pp);
            for (int i = 0; i < 12; ++i) {
                fx->setParam(i, q[0x19 + i]);
                if (applied) applied[i] = q[0x19 + i];
            }
        } else if (type != 128) {
            warnings_.push_back("effect '" + name + "': type " + std::to_string(type) + " not ported yet (bypassed)");
        }
        return fx;
    };

    for (int ri = 0; ri < int(patch.elements.size()); ++ri) {
        const auto& r = patch.elements[ri];
        Elem el;
        el.orig = ri;
        // FUN_10016f70 loader fixups on the stored element params (before the setter runs). The old-patch
        // Note-on Click scaling (0x7d * 0.3) is done once in Patch::parse; here: Analog elements get params
        // 0x19..0x25 and 0x7d reset unless the patch type (patch 0x44, trunc(v*15.9), 0 is set to 5 first) is 1 or 5.
        ParamArray rp = r.params;
        if (quant(rp[1], 15.9f) == int(ElementType::Analog)) {
            int ptype = quant(pp[0x44], 15.9f);
            if (ptype == 0) ptype = 5;
            if (ptype != 1 && ptype != 5) {
                rp[0x7d] = 0.f;
                rp[0x1a] = rp[0x19] = 0.5f;
                rp[0x21] = rp[0x20] = rp[0x1b] = rp[0x25] = rp[0x24] = rp[0x23] = 0.f;
                rp[0x22] = 0.333f;  // 0x3eaa7efa
            }
        }
        el.raw = rp;
        el.p = computeElement(rp, pp, c_);
        el.quality3 = pp[0x01];
        {  // FIX: the element's version word (+0x5ec) after its 256 setter calls (verCall; 43 before them)
            int v = 43;
            for (int i = 0; i < 0x100; ++i) v += verCall(el, i, rp[i]);
            el.p.version = v;
        }
        if (el.p.type == ElementType::Effect) {
            // FIX: a muted insert is not processed but keeps its Aux Out routing (FUN_100139c0 tests +0x41c only
            // for the effect call)
            el.isEffect = true;
            el.muted = el.p.mute;
            el.fxType = quant(r.params[0x13], 128.9f);
            el.fx = makeFx(r.params, r.name, el.knobApplied);
            effectRouting(el);
            elemIndex_[ri] = int(elems_.size());
            elems_.push_back(std::move(el));
            continue;
        }
        // FIX: muted records are built too (the original builds every record; Mute only sets +0x420 / +0x41c, which
        // FUN_10021e20 tests first), so they can be unmuted at runtime.
        if (el.p.type != ElementType::Sample) {
            if (!elementTypeSupported(el.p.type)) {
                warnings_.push_back("element '" + r.name + "': type " + std::to_string(int(el.p.type)) +
                                    " not implemented yet");
                continue;
            }
            el.playable = true;
            elemIndex_[ri] = int(elems_.size());
            elems_.push_back(std::move(el));
            continue;
        }
        bindSample(el);
        if (!el.bank) {
            warnings_.push_back("element '" + r.name + "': missing sample bank " + std::to_string(el.p.bank));
            continue;
        }
        if (!el.inst && el.wave != 0x80)
            warnings_.push_back("element '" + r.name + "': missing wave " + std::to_string(el.wave) + " in sample bank " +
                                std::to_string(el.p.bank));
        el.playable = true;
        elemIndex_[ri] = int(elems_.size());
        elems_.push_back(std::move(el));
    }

    // Patch send FX records, in slot order by the present-flags (engine mode: FUN_10016f70's own / global rule).
    size_t rec = 0;
    if (eng_) loadSendFxEngine(patch);
    for (int k = 0; k < 4 && !eng_; ++k) {
        sendFx_[k] = SendFx{};
        if (!patch.sendFxFlags[k] || rec >= patch.sendFx.size()) continue;
        const auto& r = patch.sendFx[rec++];
        SendFx& s = sendFx_[k];
        s.fx = makeFx(r.params, r.name, nullptr);
        s.raw = r.params;
        s.quick[0] = quant(r.params[0x91], 7.9f);
        s.quick[1] = quant(r.params[0x92], 7.9f);
        s.routed = true;  // Aux Out "Off" still returns to the mix (TODO: confirm the mixer path for aux outs)
        s.level = r.params[0x8b] * 4.f;
        s.pan = r.params[0x8c];
        s.send1 = r.params[0x15] * r.params[0x15] * 4.f;
    }
    if (!ownSends_ && !eng_) {  // standalone only: in hs2::Engine the global part holds the global send FX
        // Global send FX. In a fresh plugin (as REAPER loads it) these come from factory combi 0
        // "Arctic Bubbles Scape": FX1 "Long Reverb", FX2 "Short Reverb" (both Hall Reverb, level 0.5, pan 0.5).
        // TODO: real combi parsing / host-provided global FX.
        static const float kLong[12] = {1.0f, 0.f, 0.625f, 0.11f, 0.76f, 0.5f, 0.54f, 0, 0, 0, 0, 0};
        static const float kShort[12] = {1.0f, 0.f, 0.f, 0.f, 0.16f, 0.5f, 0.45f, 0, 0, 0, 0, 0};
        const float* cfg[2] = {kLong, kShort};
        for (int k = 0; k < 4; ++k) {
            sendFx_[k] = SendFx{};
            if (k >= 2) continue;
            SendFx& s = sendFx_[k];
            s.fx = createEffect(1);
            s.fx->setSampleRate(c_.sr);
            for (int i = 0; i < 12; ++i) s.fx->setParam(i, cfg[k][i]);
            s.routed = true;
            s.level = 0.5f * 4.f;
            s.pan = 0.5f;
        }
    }
    // FIX: Hyperknob assignment 13: the patch setter cases of knobs 2..6 (0x0f, 0x67, 0x69, 0x6b, 0x6d with
    // part+0xd74.. == 0xc) call FUN_10010d20(0x37, 0x38, k, value) with k = 0, 2, 3, 4, 5 (knob 1 has no such call),
    // which sets knob k of the first unmuted Rotary Speaker insert. At load this runs once the elements exist
    // (verified in the emulator: 83/0..5 get knobs 0, 4, 5).
    {
        static constexpr int kFxKnob[6] = {-1, 0, 2, 3, 4, 5};
        for (int i = 1; i < 6; ++i)
            if (quant(pp[kKnobs[i].assign], 15.9f) - 1 == 0xc) setFxKnob(0x37, 0x38, kFxKnob[i], inputs_.knob[i]);
    }
    // FIX: the loader's final Hyperknob setter calls (FUN_10016f70 end: 0x0e, 0x0f, 0x67, 0x69, 0x6b, 0x6d) as far as
    // the element versions go: quick-knob elements set 0x0e / 0x0f (re-setting their destinations), then the knob's
    // macro slot (FUN_1000ca70) re-sets one parameter of every Receive CC element (Rotary slot 13 of knobs 2..6: an
    // effect knob, no version change). The values are unchanged, so only the version words differ.
    for (int k = 0; k < 6; ++k) {
        for (int ei = 0; ei < int(elems_.size()); ++ei) {
            Elem& el = elems_[ei];
            const bool a = quant(el.raw[0x91], 7.9f) == k + 1, b = quant(el.raw[0x92], 7.9f) == k + 1;
            if (a) el.p.version += verCall(el, 0x0e, el.raw[0x0e]);
            if (b) el.p.version += verCall(el, 0x0f, el.raw[0x0f]);
            if ((a || b) && el.isEffect) elementKnobsToFx(ei);  // the routed effect knobs with the Hyperknob values
        }
        const int slot = quant(pp[kKnobs[k].assign], 15.9f) - 1;
        if (slot < 0 || (k >= 1 && slot == 0xc)) continue;
        for (auto& el : elems_)
            if (el.simRcc && slot == 6) el.p.version += verCall(el, 0x73, el.raw[0x73]);
        for (auto& el : elems_) {
            if (!el.simRcc) continue;
            const int par = macroParam(slot, el);
            el.p.version += verCall(el, par, el.raw[par]);
        }
    }

    // The arpeggiator part of the patch load (FUN_10016f70): the setter for every stored value within 0..1, in
    // index order (0x18 Gate = 0.5 for patches older than 0xBEC874C), then the blob trailer.
    {
        auto inRange = [](float v) { return v >= 0.f && v <= 1.f; };
        const ParamArray& raw = patch.params;
        for (int i = 0x11; i <= 0x21; ++i) {
            float v = raw[i];
            if (i == 0x18 && int32_t(patch.version) < 0xBEC874C) v = 0.5f;
            if (!inRange(v)) continue;
            if (i == 0x1a) continue;
            if (i == 0x1b) {
                setParam(0, 0x1b, v);
                continue;
            }
            arp_->setParam(i - 0x11, v);
        }
        if (inRange(raw[0x95]) && quant(raw[0x95], 64.9f) == 0 && !phraseSlot0_.empty())
            arp_->setPhraseObject(phraseSlot0_);
        for (int i = 0xac; i < 0xfc; ++i)
            if (inRange(raw[i])) arp_->setStep(i, raw[i]);
        if (patch.tailMarker == 0) {  // setter (0x95, 0) -> FUN_10016e20(0), then (0x1e, 1)
            if (!phraseSlot0_.empty()) arp_->setPhraseObject(phraseSlot0_);
            arp_->setParam(0x1e - 0x11, 1.f);
        } else if (patch.tailMarker == 2) {  // embedded phrase slots: slot 0 through FUN_10016e20(0), then 0x95
            const auto tr = Arp::patchTrailer(patch.blob);
            for (size_t k = 1; k < tr.slots.size() && k < phraseSlots_.size(); ++k)
                phraseSlots_[k].assign(tr.slots[k].begin(), tr.slots[k].end());
            if (!tr.slots.empty() && !tr.slots[0].empty()) {
                phraseSlot0_.assign(tr.slots[0].begin(), tr.slots[0].end());
                arp_->setPhraseObject(phraseSlot0_);
            }
            const float sc = patch_.params[0x95];
            selectScene(inRange(sc) ? quant(sc, 64.9f) : 0);  // FIX: any stored scene, not only slot 0
        }
    }
}

// FUN_10016e20(i): makes phrase slot i (part+0xc48[i]) current. Slot 0 hands its phrase (+0x1f0) to the arp; another
// slot first applies its stored Hyperknob values (flags +0x28 bit 0: obj+0x2c.., patch 0x0e/0x0f/0x67/0x69/0x6b/0x6d)
// and arp params / steps (bit 1: obj+0x5c = patch 0x11..0x21 except 0x12 / 0x1a / 0x1b, obj+0xac = steps 0xac..0xfb)
// through the setter, then hands its phrase over only when its event count (+0x1f4) is non-zero. Factory patches
// store one slot only; other slots come from user patches (FUN_10016c80 stores a scene).
void PatchPlayer::selectScene(int i) {
    if (i < 0 || i >= int(phraseSlots_.size())) return;
    const std::vector<uint8_t> obj = i == 0 ? phraseSlot0_ : phraseSlots_[size_t(i)];  // copy: setters may run
    if (obj.empty()) return;
    if (i != 0) {
        auto f = [&](size_t o) { return o + 4 <= obj.size() ? rdf(&obj[o]) : 0.f; };
        const uint8_t flags = obj.size() > 0x28 ? obj[0x28] : 0;
        if (flags & 1) {
            static constexpr int kKnob[6] = {0x0e, 0x0f, 0x67, 0x69, 0x6b, 0x6d};
            for (int k = 0; k < 6; ++k) setParam(0, kKnob[k], f(0x2c + 4 * size_t(k)));
        }
        if (flags & 2) {
            for (int idx = 0x11; idx <= 0x21; ++idx)
                if (idx != 0x1a && idx != 0x1b && idx != 0x12) setParam(0, idx, f(0x5c + 4 * size_t(idx - 0x11)));
            for (int k = 0; k < 0x50; ++k) setParam(0, 0xac + k, f(0xac + 4 * size_t(k)));
        }
        if (obj.size() < 0x1f8 || rd32(&obj[0x1f4]) == 0) return;
    }
    arp_->setPhraseObject(obj);
}

// FUN_10010d20: the first element that is an unmuted insert of type t1 or t2 gets knob k (element param 0x19 + k).
// The new value goes through the element setter, so the element's own quick-knob routes apply to it.
void PatchPlayer::setFxKnob(int t1, int t2, int k, float v) {
    for (auto& el : elems_) {
        if (!el.isEffect || el.muted || (el.fxType != t1 && el.fxType != t2)) continue;
        el.raw[0x19 + k] = v;
        if (el.fx) {
            const float q = elementKnobParams(el.raw, patch_.params)[0x19 + k];
            el.fx->setParam(k, q);
            el.knobApplied[k] = q;
        }
        return;
    }
}

// FUN_1000ef10: host tempo / transport sync of the part.
void PatchPlayer::syncTempo(double bpm, double bps, double bpb, double ppq, double barStart, int flags) {
    (void)bpb;
    bpmD_ = bpm;                                // +0xd90
    bpsD_ = bps;                                // +0xd88
    c_.tempoFactor = float(120.0 / bpm);        // +0x954 (fdiv by the unrounded bpm)
    inputs_.tempoFactor = c_.tempoFactor;
    bpm_ = float(bpm);                          // +0x958
    bps_ = float(bps);                          // +0x95c
    // vtable +0xc of every insert and own send FX (the 68 element slots from part+0x1010), and (the global part,
    // FUN_1007baf0's first call) the global send FX
    for (auto& el : elems_)
        if (el.isEffect && el.fx) el.fx->setTempo(bpm_, bps_);
    for (auto& s : sendFx_)
        if (s.fx) s.fx->setTempo(bpm_, bps_);
    arp_->sync(flags, bpm, ppq);  // @1000efb4: FUN_10045710(flags, bpm, ppq)
    // patch LFOs in Tempo sync: rate = (float)bpm * +0xdec; Tempo + Beat also sets the phase from the song position:
    // 2 * fmod((ppq - barStart) * noteValue + Phase * 1.1111, 1)
    auto lfo = [&](Lfo& l, LfoParams& p) {
        if (p.sync <= 2) return;
        p.rate = float(double(bpm_) * double(p.tempoRate));
        if (p.sync <= 3) return;
        const double y = (ppq - barStart) * double(p.noteVal) + double(p.phase) * double(1.1111f);
        const double ph = std::fmod(y, 1.0);
        l.init(float(ph + ph), p.wave, rng_);
    };
    lfo(lfo1_, lfo1P_);
    lfo(lfo2_, lfo2P_);
    // playing voices with a tempo-synced LFO3: +0x2c4 = (float)bpm * elem+0x4c4
    for (int i = 0; i < count_; ++i) {
        Voice* v = voices_[list_[i].slot].get();
        if (!v) continue;
        const auto& e = elems_[list_[i].elem].p;
        if (e.lfo3.sync > 2) v->setLfo3Rate(float(double(bpm_) * double(e.lfo3.tempoRate)));
    }
}

// FUN_100884c0 (plugin, per host block): reads VstTimeInfo; on a tempo, time signature or transport change it calls
// FUN_1007baf0 -> FUN_1000ef10 for every part with flags (0x20 tempo, +2 stop / +4 start, 0x48 signature (0x50 when
// stopped: the position snaps to the bar start), +8 jump > 0.2 beats, +1 playing). Returns the song position.
double PatchPlayer::hostSync(const HostTransport* t) {
    if (!t) return hPos_;
    int f = 0;
    if (t->tempoValid && hBpm_ != t->bpm) {
        hBpm_ = t->bpm;
        f = 0x20;
        hBps_ = hBpm_ * hSpb_;
    }
    const bool playing = t->playing;
    if (hPlaying_ != playing) {
        hPlaying_ = playing;
        f += (playing ? 2 : 0) + 2;
    }
    int f6 = f;
    if (t->sigValid && t->sigDen != 0) {
        const double bpb = double(t->sigNum) * 4.0 / double(t->sigDen);
        if (hBpb_ != bpb) {
            hBpb_ = bpb;
            f6 = f + 0x48;
            if (t->barValid) {
                hBar_ = t->barStart;
                if (!hPlaying_) {
                    hPos_ = t->barStart;
                    f6 = f + 0x50;
                }
            }
        }
    }
    if (hPlaying_ && t->ppqValid) {
        if (std::fabs(hPos_ - t->ppq) > double(0.2f)) f6 += 8;
        hPos_ = t->ppq;
        if (hPos_ < hBar_) {
            do hBar_ -= hBpb_;
            while (hPos_ < hBar_);
        }
    }
    if (f6 != 0) {
        if (hPlaying_) f6 += 1;
        // FUN_1007baf0: sync the parts and keep the values for parts loaded later (synth+0x128..0x148)
        syncTempo(hBpm_, hBps_, hBpb_, hPos_, hBar_, f6);
        sBpm_ = hBpm_;
        sBps_ = hBps_;
        sBpb_ = hBpb_;
        sBar_ = hBar_;
        sFlags_ = (f6 & 1) | 0x60;
    }
    return hPos_;
}

void PatchPlayer::processBlock(float* out, int frames, const MidiEvent* events, int numEvents,
                               const HostTransport* transport) {
    if (frames <= 0) return;
    double pos = hostSync(transport);
    mix_.assign(size_t(frames) * 2, 0.f);
    for (int k = 0; k < 4; ++k) {
        globalBus_[k].assign(size_t(frames) * 2, 0.f);
        globalUsed_[k] = false;
    }
    // FUN_1007bcf0: render up to each event, then send it (status & 0xf0) to the part's MIDI handler
    int at = 0;
    for (int i = 0; i < numEvents; ++i) {
        const int o = events[i].offset;
        if (o >= frames) break;
        if (at < o) {
            partRender(&mix_[size_t(at) * 2], o - at, pos, at);
            pos = double(o - at) * sBps_ + pos;
            at = o;
        }
        uint8_t m[4];
        std::memcpy(m, events[i].data, 4);
        m[0] &= 0xf0;
        midiEvent(m, false);
    }
    if (at < frames) partRender(&mix_[size_t(at) * 2], frames - at, pos, at);
    // Global send FX (the global part's FUN_1000f130 with offset -1), once for the whole block
    if (!ownSends_)
        for (int k2 = 3; k2 >= 0; --k2) {
            SendFx& s = sendFx_[k2];
            if (!s.fx) continue;
            bool sil = !globalUsed_[k2];
            s.fx->processGated(globalBus_[k2].data(), frames, sil);
            if (sil) continue;
            if (k2 != 0 && s.send1 > 1e-8f) {
                addScaled(globalBus_[0].data(), globalBus_[k2].data(), frames, s.send1, s.send1);
                globalUsed_[0] = true;
            }
            if (s.routed) {
                const float R = s.level * s.pan, L = s.level - R;
                addScaled(mix_.data(), globalBus_[k2].data(), frames, L, R);
            }
        }
    if (enhMode_) {
        for (int ch = 0; ch < 2; ++ch) {
            Enhancer& e = enh_[ch];
            for (int n = 0; n < frames; ++n) {
                float& x = mix_[2 * n + ch];
                const float in = x;
                float& carry = e.carry[enhMode_ == 2 ? e.phase : 0];  // mode 2: two-sample lag
                x = (in + carry) * enhG_ + in;
                e.s1 += (in - e.s1) * enhA_;
                e.s2 += (e.s1 - e.s2) * enhA_;
                carry = e.s2 - in;
                e.phase ^= 1;
            }
        }
    }
    for (size_t i = 0; i < mix_.size(); ++i) out[i] += mix_[i];
    hPos_ = double(frames) * hBps_ + hPos_;  // FUN_1008d400: +0xb20 += frames * beats per sample
}

// FUN_100191b0: one part render call; the arpeggiator splits it at its events (FUN_100485d0).
bool PatchPlayer::partRender(float* out, int frames, double songPos, int sendOfs) {
    uint8_t ev[4] = {0, 0, 0, 0};  // once per call: a call that pops nothing resends it (original quirk)
    int r = arp_->next(frames, songPos, ev);
    while (r < frames) {
        if (r > 0) {
            renderCall(out, r, songPos, sendOfs);
            if (out) out += size_t(r) * 2;
            sendOfs += r;
            frames -= r;
            songPos = double(r) * bpsD_ + songPos;
        }
        if (ev[0]) {
            static const bool log = std::getenv("HS2_ARPLOG") != nullptr;
            if (log) std::fprintf(stderr, "arp %lld %02x %02x %02x %02x\n", (long long)dbgClock_, ev[0], ev[1], ev[2], ev[3]);
            midiEvent(ev, true);
        }
        r = arp_->next(frames, songPos, ev);
    }
    return renderCall(out, frames, songPos, sendOfs);
}

// ---------------------------------------------------------------------------------------------------------------
// MIDI (FUN_10017880)

void PatchPlayer::midiEvent(uint8_t* m, bool fromArp) {
    asleep_ = false;  // part+0x1140
    const int st = m[0] & 0xf0;
    if (!fromArp) {
        // host events: a note-on outside the patch key / velocity range is dropped (also a 0x90 with velocity 0)
        if (st == 0x90 && int(partVelGain_[m[2] & 127]) * int(partKeyGain_[m[1] & 127]) == 0) return;
        if (arp_->midiIn(m)) return;  // FUN_10046980: the arpeggiator takes notes, CC64, ...
    }
    switch (st) {
        case 0x80:
            m[2] = 0;  // the original clears the velocity byte of the event it was given
            noteOffEvent(m[1]);
            break;
        case 0x90:
            if (m[2] == 0) noteOffEvent(m[1]);
            else noteOnEvent(m[1], m[2], int8_t(m[3]), fromArp);
            break;
        case 0xb0: controller(m[1], m[2]); break;
        case 0xd0: atRaw_ = float(double(m[1]) * double(0.007874016f) + double(1e-8f)); break;  // +0xda8
        case 0xe0: pitchBend(m[1], m[2]); break;
        default: break;  // poly pressure, program change: nothing in the part
    }
}

void PatchPlayer::pitchBend(int lsb, int msb) {
    const double f = double((msb - 64) * 128 + lsb) * double(0.00012207031f);
    inputs_.pitchBend = float(f);  // +0xbcc
    // PB Range (bend up) / PB Down (+0xbb8 / +0xbbc, trunc(v*15.9)): semitones; 13 = whole steps of 12 (rounded),
    // 14 = a 7-step harmonic scale, 15 = 24 semitones
    const int range = f >= 0.0 ? pbUp_ : pbDown_;
    if (range == 13) {
        bendSemis_ = float(std::floor(double(float(f * double(12.f) + double(0.5f)))));
    } else if (range == 14) {
        const double k0 = f * double(0.45f);
        const int step = int(double(inputs_.pitchBend) * double(3.f) + double(3.5f));
        double r;
        switch (step) {
            case 0: r = k0 - double(11.55f); break;
            case 1: r = k0 - double(6.72f); break;
            case 2: r = k0 - double(3.f); break;
            case 3: r = k0; break;
            case 4: r = k0 + double(4.87f); break;
            case 5: r = k0 + double(8.84f); break;
            default: r = k0 + double(11.55f); break;
        }
        bendSemis_ = float(r);
    } else {
        bendSemis_ = float(double(range == 15 ? 24 : range) * f);
        // Patch Type 3 (organ) with both ranges 0: the bend wheel switches the Rotary Speaker (knob 2)
        if (patchType_ == 3 && pbUp_ + pbDown_ == 0) {
            if (inputs_.pitchBend > 0.25f) setFxKnob(0x37, 0x38, 2, 1.f);
            else if (inputs_.pitchBend < -0.25f) setFxKnob(0x37, 0x38, 2, 0.f);
        }
    }
    const double ofs = double(bendSemis_) * double(kSemitone) + double(inputs_.fineTune);
    inputs_.pitchOfs = float(ofs);  // +0xbd4
    if (heldCount_ != 0)
        notePitch_ = float(double(heldSorted_[heldCount_ - 1] + transpose_) * double(kSemitone) + ofs);
}

void PatchPlayer::controller(int cc, int value) {
    const float v = float(double(value) * double(0.007874016f) + double(1e-8f));
    if (cc == 1) cc1Copy_ = v;  // +0xdc8
    if (cc < 0x80 && ccMap_[cc] != -1) {  // the CC map (part+0x1a0) wins over the controller cases
        setParam(ccMap_[cc] >> 8, ccMap_[cc] & 0xff, v);
        return;
    }
    switch (cc) {
        case 1: modWheelRaw_ = v; break;     // +0xda0
        case 2: inputs_.breath = v; break;   // +0xdb0
        case 3: inputs_.wheel3 = v; break;   // +0xdb4
        case 4: inputs_.foot = v; break;     // +0xdb8
        case 0x0b: {                         // expression: +0x950 = v^2 + 1e-8, element level = +0x5b8 * that
            const float x = float(double(v) * double(v) + double(1e-8f));
            c_.expression = x;
            for (auto& el : elems_) {
                if (el.isEffect) continue;
                el.p.level = float(double(el.p.levelRaw) * double(x));
                ++el.p.version;
            }
            break;
        }
        case 0x40: {  // sustain
            for (int i = 0; i < count_; ++i)
                if (Voice* vo = voices_[list_[i].slot].get()) vo->setSustainWord(v);
            sustain_ = value >> 6;
            if (sustain_ == 0) {  // pedal up: a note-off of note -1 releases the sustained (-1) entries
                uint8_t m[4] = {0x80, 0xff, 0, 0};
                midiEvent(m, false);
            } else if (redamper_ != 0) {  // Redamper: released voices are held again with a slower release
                const float g = float(expF(float(double(redamper_) * double(-0.03f))));
                for (int i = 0; i < count_; ++i) {
                    if (list_[i].held != 0) continue;
                    list_[i].held = -1;
                    if (Voice* vo = voices_[list_[i].slot].get()) {
                        vo->scaleAmpCoef(g);
                        vo->clearReleased();
                    }
                }
            }
            break;
        }
        case 0x42:  // sostenuto: every held entry becomes sustained (not while the arpeggiator is on)
            if ((value & 0xc0) && !arp_->on())
                for (int i = 0; i < count_; ++i)
                    if (list_[i].held != 0) list_[i].held = -1;
            break;
        case 0x43: softPedal_ = float(value >> 6); break;  // +0xdbc
        case 0x44: legatoPedal_ = value >> 6; break;       // +0xd9c
        case 0x54: {  // portamento control: every element's last glide target = the given note
            const float g = float(double((value - 60) * 100) * double(kCent));
            for (auto& el : elems_) el.lastGlide = g;
            break;
        }
        case 0x78:
        case 0x7b:  // all sound / notes off: every listed voice gets the mute fadeout
            for (int i = 0; i < count_; ++i) stealEntry(i);
            break;
        case 0x79:  // reset all controllers (the element levels keep the old expression)
            legatoPedal_ = sustain_ = 0;
            bendSemis_ = 0.f;
            inputs_.pitchBend = 0.f;
            softPedal_ = 0.f;
            inputs_.aftertouch = atRaw_ = 0.f;
            inputs_.modWheel = modWheelRaw_ = 0.f;
            cc1Copy_ = 0.f;
            c_.expression = 1.f;
            inputs_.pitchOfs = inputs_.fineTune;
            break;
        case 0x7e:
        case 0x7f:  // mono / poly mode messages -> patch Mono/Poly
            setParam(0, 0x39, (cc == 0x7e && value > 0) ? 1.f : 0.f);
            break;
        case 0x80:  // arpeggiator Seq1 / Seq2 values (internal controller numbers)
            seq1_ = float(double(value - 63) * double(0.015873f));
            if (patchType_ != 2) inputs_.seq1 = seq1_;
            break;
        case 0x81: inputs_.seq2 = float(double(value - 63) * double(0.015873f)); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Parameter changes at runtime (FUN_100121a0 / FUN_100221f0)

void PatchPlayer::reapplyElement(int ei, int times) {
    Elem& el = elems_[ei];
    if (el.isEffect) return;
    const int ver = el.p.version + times;  // +0x5ec counts setter calls; voices refresh when it differs
    auto old = el.p.typeParams;
    const uint32_t mv = el.p.modVersion;
    el.p = computeElement(el.raw, patch_.params, c_);
    el.p.version = ver;
    el.p.modVersion = mv;
    if (el.quality3 != patch_.params[0x01]) {  // the ECO/XXL word (+0x434) keeps the quality of its case-3 call
        const int quality = quant(el.quality3, 2.9f);
        el.p.eco = quality == 0 || (quality != 2 && el.raw[0x03] <= 0.5f);
    }
    // voices of other types keep a raw pointer to the old type data: keep it alive
    if (old && old != el.p.typeParams) el.retired.push_back(std::move(old));
}

// An insert's Aux Out, output level / pan (2 * elem+0x5b8, +0x5d8) and send levels as the part render reads them:
// setter values, i.e. after the element quick-knob routes (FUN_1001c440 k < 0). FIX: drum kits route Hyperknobs to
// their group buses' Output Level (e.g. 3/65 TOM GROUP: knob A = Hyperknob 6 -> 0x8b).
void PatchPlayer::effectRouting(Elem& el) {
    const ParamArray q = elementKnobParams(el.raw, patch_.params);
    el.auxOut = quant(q[0x14], 15.9f);
    el.outGain = 4.f * q[0x8b] * q[0x8b];
    el.pan = q[0x8c];
    for (int k = 0; k < 4; ++k) el.send[k] = q[0x15 + k] * q[0x15 + k] * 4.f;
}

void PatchPlayer::elementKnobsToFx(int ei) {
    Elem& el = elems_[ei];
    effectRouting(el);
    if (!el.fx) return;
    const ParamArray q = elementKnobParams(el.raw, patch_.params);
    for (int i = 0; i < 12; ++i)
        if (q[0x19 + i] != el.knobApplied[i]) {
            el.fx->setParam(i, q[0x19 + i]);
            el.knobApplied[i] = q[0x19 + i];
        }
}

// FUN_1000ca70(slot, weight): every element with Receive CC/EE gets its macro parameter re-set (slot 1 also the
// patch LFO rates, slot 6 also 0x73).
void PatchPlayer::macro(int slot) {
    if (slot == 1) {  // cases 0x57 / 0x5b again: the free rates (state kept)
        const LfoParams a = lfo1P_, b = lfo2P_;
        computePatchLfos();
        lfo1P_.wave = a.wave;
        lfo2P_.wave = b.wave;
    }
    for (int ei = 0; ei < int(elems_.size()); ++ei) {
        Elem& el = elems_[ei];
        if (el.isEffect || !(el.raw[0x02] > 0.5f)) continue;
        int b = slot == 6 ? verCall(el, 0x73, el.raw[0x73]) : 0;
        const int par = macroParam(slot, el);
        b += verCall(el, par, el.raw[par]);
        reapplyElement(ei, b);
    }
}

// FUN_1000ca70: slot -> the parameter the Receive CC elements re-set (slot 0 / default 0x62, which a Sample element
// replaces by 0x16; slot 6 also 0x73 first).
int PatchPlayer::macroParam(int slot, const Elem& el) const {
    int p;
    switch (slot) {
        case 1: p = 0x57; break;
        case 2: p = 0x6a; break;
        case 3: p = 0x68; break;
        case 4: p = 0x6e; break;
        case 5: p = 0x6f; break;
        case 6: p = 0x70; break;
        case 7: p = 0x7e; break;
        case 8: p = 0x7f; break;
        case 9: p = 0x82; break;
        case 10: p = 0x43; break;
        case 11: p = 0x4e; break;
        case 13: p = 0x80; break;
        case 14: p = 0x71; break;
        case 0x1e: p = 0x39; break;
        case 0x1f: p = 3; break;
        default: p = 0x62; break;
    }
    if (p == 0x62 && el.simType == 2) p = 0x16;
    return p;
}

// Element setter version bumps (+0x5ec). Measured in the emulator for every type and parameter (tools: scratch
// bump_table.py), all unconditional: the common cases 0x46/0x47/0x48 (pitch, FUN_1001c410), 0x4e, 0x57, 0x62, 0x66,
// 0x68, 0x6a, 0x6b, 0x6e, 0x8b, 0x8c, and the type cases (Drawbar 0x19..0x21, Wavetable 0x15..0x18 / 0x1d, Analog
// 0x15..0x17 / 0x19 / 0x1a / 0x1d..0x21 / 0x23 / 0x24). Nested calls: case 1 (type change: 0x11..0x30 again),
// case 2 (Receive CC change: 0x62 0x6a 0x68 0x7e 0x7f 0x82 0x6e 0x6f 0x70 0x39), case 6 (a quick-knob destination:
// the knob before and after the change), cases 7/8/9/10 (the knob's two destinations). With a new element's state
// (type 0, Receive CC 1, destinations OFF) and base 43 this gives the original's loaded versions exactly (20 patches,
// 99 elements checked, scratch vermodel2.py).
int PatchPlayer::verCall(Elem& el, int idx, float v) {
    switch (idx) {
        case 1: {
            const int nt = quant(v, 15.9f);
            if (nt == el.simType) return 0;
            el.simType = nt;
            int n = 0;
            for (int i = 0x11; i < 0x31; ++i) n += verCall(el, i, el.raw[i]);
            return n;
        }
        case 2: {
            const bool r = v > 0.5f;
            if (r == el.simRcc) return 0;
            el.simRcc = r;
            int n = 0;
            for (int i : {0x62, 0x6a, 0x68, 0x7e, 0x7f, 0x82, 0x6e, 0x6f, 0x70, 0x39}) n += verCall(el, i, el.raw[i]);
            return n;
        }
        case 6: case 8: case 0x0a: case 0x0c: {
            const int k = (idx - 6) / 2, knob = 0x0e + k / 2;
            int n = verCall(el, knob, el.raw[knob]);
            el.simDest[k] = 0x10 + int(double(v) * double(235.9f));
            return n + verCall(el, knob, el.raw[knob]);
        }
        case 7: case 9: case 0x0e:
            return verCall(el, el.simDest[0], 0.f) + verCall(el, el.simDest[1], 0.f);
        case 0x0b: case 0x0d: case 0x0f:
            return verCall(el, el.simDest[2], 0.f) + verCall(el, el.simDest[3], 0.f);
        default: break;
    }
    if (idx < 0 || idx >= 0xac) return 0;
    if (idx >= 0x11 && idx <= 0x30) {
        switch (el.simType) {
            case 1: return idx >= 0x19 && idx <= 0x21;
            case 3: return (idx >= 0x15 && idx <= 0x18) || idx == 0x1d;
            case 5: return (idx >= 0x15 && idx <= 0x17) || idx == 0x19 || idx == 0x1a || (idx >= 0x1d && idx <= 0x21) ||
                           idx == 0x23 || idx == 0x24;
            default: return 0;
        }
    }
    switch (idx) {
        case 0x46: case 0x47: case 0x48: case 0x4e: case 0x57: case 0x62: case 0x66: case 0x68: case 0x6a: case 0x6b:
        case 0x6e: case 0x8b: case 0x8c: return 1;
        default: return 0;
    }
}

void PatchPlayer::setKnob(int k, float x) {
    const auto& pp = patch_.params;
    inputs_.knob[k] = (x - pp[kKnobs[k].ofs]) * pp[kKnobs[k].rng];  // +0xd58 + 4k
    // elements whose quick knob A / B takes this Hyperknob (0x91 / 0x92 source = k + 1): setter 0x0e / 0x0f
    for (int ei = 0; ei < int(elems_.size()); ++ei) {
        Elem& el = elems_[ei];
        const bool a = quant(el.raw[0x91], 7.9f) == k + 1, b = quant(el.raw[0x92], 7.9f) == k + 1;
        if (!a && !b) continue;
        if (el.isEffect) {
            elementKnobsToFx(ei);
            continue;
        }
        const int n = (a ? verCall(el, 0x0e, el.raw[0x0e]) : 0) + (b ? verCall(el, 0x0f, el.raw[0x0f]) : 0);
        reapplyElement(ei, n);
    }
    // original quirk: of the send FX only slots 3 and 4 (part+0x1118, two entries) are checked
    for (int s = 2; s < 4; ++s) {
        SendFx& sf = sendFx_[s];
        if (!sf.fx || (sf.quick[0] != k + 1 && sf.quick[1] != k + 1)) continue;
        const ParamArray q = elementKnobParams(sf.raw, pp);
        for (int i = 0; i < 12; ++i) sf.fx->setParam(i, q[0x19 + i]);
    }
    const int slot = quant(pp[kKnobs[k].assign], 15.9f) - 1;
    if (slot < 0) return;
    static constexpr int kFxKnob[6] = {-1, 0, 2, 3, 4, 5};
    if (k != 0 && slot == 0xc) setFxKnob(0x37, 0x38, kFxKnob[k], inputs_.knob[k]);
    else macro(slot);
}

void PatchPlayer::setParam(int elem, int idx, float value) {
    float x = value;  // values outside 0..1 snap to 1 (> 0.5) or 0
    if (value < 0.f || value > 1.f) x = value > 0.5f ? 1.f : 0.f;
    if (elem >= 0x41 && elem <= 0x44) {  // a send FX slot (part+0x1110 + 4k)
        if (idx >= 0 && idx <= 0xff) setSendFxParam(elem - 0x41, idx, x);
        return;
    }
    if (elem != 0) {  // FUN_100221f0 on element record elem - 1
        const int rec = elem - 1;
        if (rec < 0 || rec >= int(elemIndex_.size()) || elemIndex_[rec] < 0 || idx < 0 || idx > 0xff) return;
        const int ei = elemIndex_[rec];
        Elem& el = elems_[ei];
        el.raw[idx] = x;
        if (el.isEffect) {
            const ParamArray& r = el.raw;
            el.muted = r[0] > 0.5f;  // case 0: +0x41c (an effect ignores its Output Level here)
            elementKnobsToFx(ei);
        } else {
            const uint32_t mv = el.p.modVersion;
            if (idx == 0x03) el.quality3 = patch_.params[0x01];  // case 3 reads the current part quality
            reapplyElement(ei, verCall(el, idx, x));
            el.p.modVersion = mv;
            if (el.p.type == ElementType::Sample) bindSample(el);  // cases 0x7d..0x7f (piano layer, bank, wave)
            if (idx >= 0xac && idx < 0xfc) {  // a mod-matrix row: FUN_1001c570
                const int row = (idx - 0xac) / 5, field = (idx - 0xac) % 5;
                el.p.modVersion = (mv & ~0xffu) + 0x100u + uint32_t(row);
                if (field == 0 || field == 1 || field == 3) modReset_ = true;  // part+0x978 = 1.0
            }
        }
        return;
    }
    if (idx < 0 || idx >= 0xfc) return;
    patch_.params[idx] = x;
    auto& pp = patch_.params;
    switch (idx) {
        case 0x00: partMute_ = x > 0.5f; break;
        case 0x01:  // part quality (+0x94c): the Receive CC/EE elements re-run their ECO/XXL case 3 (no version bump)
            for (int ei = 0; ei < int(elems_.size()); ++ei)
                if (!elems_[ei].isEffect && elems_[ei].raw[0x02] > 0.5f) {
                    elems_[ei].quality3 = x;
                    reapplyElement(ei, 0);
                }
            break;
        case 0x3d: partOutput_ = std::min(quant(x, 32.9f), eng_ ? eng_->outputs : 1); break;  // +0xba8
        case 0x3e:  // FX mode (+0x2b0): the sends of a slot with an own send FX go to it (engine mode)
            if (eng_) ownSends_ = x > 0.5f;
            break;
        case 0x03: {  // patch Glide (part+0xbc0), read by the note-on / legato
            const int g = quant(x, 3.9f);
            for (auto& el : elems_) el.p.patchGlide = g;
            break;
        }
        case 0x0e: setKnob(0, x); break;
        case 0x0f: setKnob(1, x); break;
        case 0x67: setKnob(2, x); break;
        case 0x69: setKnob(3, x); break;
        case 0x6b: setKnob(4, x); break;
        case 0x6d: setKnob(5, x); break;
        case 0x31: case 0x32: case 0x33: buildXfade(partKeyGain_, pp[0x31], pp[0x32], pp[0x33]); break;
        case 0x34: case 0x35: case 0x36: buildXfade(partVelGain_, pp[0x34], pp[0x35], pp[0x36]); break;
        case 0x37: transpose_ = quant(pp[0x37] * 72.f + 0.5f, 1.f) - 36; break;
        case 0x38:
            inputs_.fineTune = float(((double(x + x) - 1.0) + double(masterTune_)) * double(kSemitone));
            inputs_.pitchOfs = float(double(bendSemis_) * double(kSemitone) + double(inputs_.fineTune));
            break;
        case 0x39:  // patch Mono/Poly: the Receive CC/EE elements re-set their Mono Mode
            for (int ei = 0; ei < int(elems_.size()); ++ei)
                if (!elems_[ei].isEffect && elems_[ei].raw[0x02] > 0.5f)
                    reapplyElement(ei, verCall(elems_[ei], 0x39, elems_[ei].raw[0x39]));
            break;
        case 0x3a: poly_ = 1 + quant(x, 63.9f); break;
        case 0x3b: case 0x3c: case 0x43: updateOutputGain(); break;
        case 0x3f: case 0x40: case 0x41: case 0x42: patchSend_[idx - 0x3f] = x * x; break;
        case 0x45: redamper_ = quant(x, 127.9f); break;
        case 0x47:
            pianoChar_ = float(double(x) - double(0.375f));
            if (patchType_ == 2) inputs_.seq1 = pianoChar_;
            break;
        case 0x48: case 0x4b: case 0x54:  // @100122f4: part+0x974, then element records 0..7 re-set Wave (0x13)
            computePianoSelect();
            for (int rec = 0; rec < 8 && rec < int(elemIndex_.size()); ++rec) {
                const int ei = elemIndex_[rec];
                if (ei >= 0 && !elems_[ei].isEffect && elems_[ei].p.type == ElementType::Sample) bindSample(elems_[ei]);
            }
            break;
        case 0x4c: case 0x4d: case 0x50: case 0x51: {  // FUN_10010770: reload when the bank / entry index changes
            const int k = idx >= 0x50, v = quant(x, 128.9f);
            int& slot = (idx & 1) ? tuneEntry_[k] : tuneBank_[k];
            if (v != slot) {
                slot = v;
                loadTuning(k);
            }
            break;
        }
        case 0x4e: case 0x52: tuneRoot_[idx == 0x52] = int(double(x) * 24.0 + 0.5) - 12; break;
        case 0x53: {
            const double d = double(x) - 0.5;
            tuneDepth_ = float(std::fabs(d) * d * 16.0);
            break;
        }
        case 0x49: pbUp_ = quant(x, 15.9f); break;
        case 0x4a: pbDown_ = quant(x, 15.9f); break;
        case 0x70: case 0x73: case 0x76: case 0x7b: case 0x8a: case 0x8d:
        case 0x71: case 0x72: case 0x74: case 0x75: case 0x77: case 0x78: case 0x7c: case 0x7d:
        case 0x8b: case 0x8c: case 0x8e: case 0x8f:  // FUN_1000cc40: knob values and macro slots, no element re-set
            for (int i = 0; i < 6; ++i)
                inputs_.knob[i] = (pp[kKnobs[i].value] - pp[kKnobs[i].ofs]) * pp[kKnobs[i].rng];
            break;
        case 0x7e: case 0x7f: case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86:
        case 0x87: macro(idx - 0x7e); break;  // macro weight 2v - 1
        case 0x94:  // Glide Time weight (slot 11): the Receive CC/EE elements re-set their Glide Time
            for (int ei = 0; ei < int(elems_.size()); ++ei)
                if (!elems_[ei].isEffect && elems_[ei].raw[0x02] > 0.5f)
                    reapplyElement(ei, verCall(elems_[ei], 0x4e, elems_[ei].raw[0x4e]));
            break;
        case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
        case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20: case 0x21:
            arp_->setParam(idx - 0x11, x);  // @10013435
            break;
        case 0x1b: {  // phrase bank / program (@10013445): load the entry as slot 0 (FUN_10016e20(0))
            const int bank = quant(pp[0x1a], 128.9f), prog = quant(pp[0x1b], 128.9f);
            if (bank + prog == 0) break;
            auto obj = Arp::loadPhraseEntry(content_.root(), bank, prog);
            if (obj.empty()) break;
            phraseSlot0_ = std::move(obj);
            arp_->setPhraseObject(phraseSlot0_);
            break;
        }
        case 0x95: selectScene(quant(x, 64.9f)); break;  // current scene FUN_10016e20(trunc(v*64.9))
        default:
            if (idx >= 0xac && idx < 0xfc) arp_->setStep(idx, x);  // arp step records
            break;  // TODO: LFOs, scales: not reachable by the default CC map
    }
}

// FUN_1001bb80(4, bank, entry): the tuning entry of the user's "Tunings/NNN ..." bank (kind 4 'T'), read at runtime.
// Entry slot 1 = int32 count + count floats (cents). A missing bank or entry leaves no tuning (null handle).
void PatchPlayer::loadTuning(int k) {
    Tuning& t = tuning_[k];
    t = Tuning{};
    const int bank = tuneBank_[k], entry = tuneEntry_[k];
    if (bank < 0 || bank >= 0x80 || entry < 0) return;
    char prefix[8];
    std::snprintf(prefix, sizeof prefix, "%03d ", bank);
    try {
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(content_.root() / "Tunings", ec)) {
            const auto name = f.path().filename().string();
            if (!f.is_regular_file() || name.compare(0, 4, prefix) != 0) continue;
            auto mb = MilanBank::open(f.path());
            if (entry >= int(mb->entries.size())) return;
            auto s = mb->read(mb->entries[size_t(entry)].slot1);
            while (s.size() > 31 && std::memcmp(s.data(), "SyncrosoftContentHeader", 23) == 0) {  // FUN_10083a50
                const size_t skip = 8 + size_t(rd32(s.data() + 27));
                if (skip >= s.size()) return;
                s = s.subspan(skip);
            }
            if (s.size() < 4) return;
            t.count = int32_t(rd32(s.data()));
            for (size_t i = 4; i + 4 <= s.size(); i += 4) t.cents.push_back(rdf(s.data() + i));
            t.loaded = true;
            return;
        }
    } catch (const std::exception&) {
        t = Tuning{};
    }
}

// FUN_10010870(note, &scale2): tuning 2 (part+0xbf8, root +0xbec) times its depth (+0xbfc) plus tuning 1 (+0xbf4, root
// +0xbe0), in cents, as the x87 register value. Counts 0..100 wrap the note (C remainder), others clamp to 0..count.
// *scale2 = 0.01 * tuning 2's value (voice+0x310, mod source 20), 0 without tuning 2.
double PatchPlayer::tuneCents(int note, float* scale2) const {
    auto value = [](const Tuning& t, int i) -> float {  // piVar2[i + 1]
        if (t.count >= 0 && t.count < 0x65) {
            if (t.count == 0) return 0.f;  // the original divides by zero
            i %= t.count;
        } else {
            if (t.count <= i) i = t.count;
            if (i < 0) i = 0;
        }
        if (i == -1) return std::bit_cast<float>(uint32_t(t.count));  // the count word itself
        return i >= 0 && i < int(t.cents.size()) ? t.cents[size_t(i)] : 0.f;  // outside the entry: unknown, 0
    };
    double r = 0.0;
    if (!tuning_[1].loaded) {
        if (scale2) *scale2 = 0.f;
    } else {
        const float f = value(tuning_[1], note - tuneRoot_[1]);
        if (scale2) *scale2 = float(double(0.01f) * double(f));
        r = double(f) * double(tuneDepth_);
    }
    if (tuning_[0].loaded) r += double(value(tuning_[0], note - tuneRoot_[0]));
    return r;
}

// Patch setter cases 0x48 / 0x4b / 0x54 (@100122f4): part+0x974 = floor(float(Piano Mode * 2.9)) * 6
// + Piano Reso * 2.9 + (Piano Model > 0.5 ? 3 : 0), x87 order; the element setter case 0x7f reads it truncated.
void PatchPlayer::computePianoSelect() {
    const auto& pp = patch_.params;
    const double model = pp[0x48] > 0.5f ? 3.0 : 0.0;
    const float m = float(double(pp[0x54]) * double(2.9f));
    const double acc = std::floor(double(m)) * 6.0 + double(pp[0x4b]) * double(2.9f) + model;
    pianoSel_ = int(float(acc));
}

// Element setter case 0x7e (Sample Bank -> +0xbb0 bits 8..15) and case 0x7f (Wave, @10023ae6): an element with a
// piano layer (+0x854 = Sample param 0x11, trunc(v*15.9), 1..8) and a wave below 0x80 gets its instrument from the
// part's piano selection k = trunc(part+0x974): mode = k / 6 (0 ECO, 1 MID, 2 XXL), r = k % 6 (Piano Reso 0..2,
// + 3 for Model 2). The instruments of each mode (bank 009 Grand Pianos order): main Model 1 / Model 1 sustain
// resonance / main Model 2 / Model 2 sustain resonance; 0x80 = none (the layer is off).
void PatchPlayer::bindSample(Elem& el) {
    el.bank = content_.sampleBank(el.p.bank);
    int w = el.p.wave;
    const int layer = el.p.zoneVelMode;
    if (layer != 0 && w < 0x80) {
        const int mode = pianoSel_ / 6, r = pianoSel_ % 6;
        int m1 = 0, res1 = 2, m2 = 1, res2 = 3;  // XXL (ebx keeps 0)
        if (mode == 0) m1 = 8, res1 = 12, m2 = 10, res2 = 14;
        else if (mode == 1) m1 = 7, res1 = 11, m2 = 9, res2 = 13;
        switch (layer) {
            case 1: w = r < 3 ? m1 : 0x80; break;    // Model 1 main
            case 2: w = r == 1 ? res1 : 0x80; break; // Model 1 resonance, Reso "Sustain"
            case 3: w = r <= 2 ? w : 0x80; break;    // Model 1 attack (stored wave)
            case 4: w = r == 2 ? res1 : 0x80; break; // Model 1 resonance, Reso "Always"
            case 5: w = r > 2 ? m2 : 0x80; break;    // Model 2 main
            case 6: w = r == 4 ? res2 : 0x80; break; // Model 2 resonance, Reso "Sustain"
            case 7: w = r >= 3 ? w : 0x80; break;    // Model 2 attack (stored wave)
            case 8: w = r == 5 ? res2 : 0x80; break; // Model 2 resonance, Reso "Always"
            default: break;
        }
    }
    el.wave = w;
    el.inst = el.bank && w < int(el.bank->instruments.size()) ? &el.bank->instruments[w] : nullptr;
}

// FUN_1001c0f0: the zone velocity of piano layers 1 / 5 is one of 4..5 fixed velocity layers; the thresholds move
// with Piano Character (part+0x970). Table rows 0..3 are for instruments whose name has '2' at index 6 ("Model 2")
// shifted by 4 rows; rows u and u + 1 are interpolated by the low byte of 0x100 - trunc(part+0x970 * -1024).
int PatchPlayer::pianoZoneVel(const Elem& el, int vel) const {
    static constexpr int16_t kTab[48] = {
        0x34, 0x49, 0x6a, 0x80, 999, 0,  0x2e, 0x45, 0x5c, 0x74, 999, 0,  0x1a, 0x31, 0x4c, 100, 999, 0,
        0, 0, 0x25, 0x5b, 999, 0,        0x50, 0x7d, 0x80, 0x80, 999, 0,  0x48, 0x66, 0x73, 0x79, 999, 0,
        0x2b, 0x4e, 0x65, 0x78, 999, 0,  0, 0, 0x40, 0x67, 999, 0};
    const bool model2 = el.inst && el.inst->name.size() > 6 && el.inst->name[6] == '2';
    int c = 0x100 - int(float(double(pianoChar_) * -1024.0));  // _ftol (truncation), compared as a short
    c = int16_t(c);
    if (c < 0) c = 0;
    else if (c > 0x300) c = 0x300;
    const int u = (c >> 8) + (model2 ? 4 : 0), f = c & 0xff;
    int16_t t[6];
    for (int j = 0; j < 4; ++j) {
        const int a = kTab[u * 6 + j];
        const int b = f ? kTab[(u + 1) * 6 + j] : a;  // row u + 1 is past the table only when f = 0
        t[j] = int16_t(((b - a) * f >> 8) + a);
    }
    t[4] = 999;
    if (vel < t[0]) return 1;
    int i = 0;
    do ++i; while (t[i] <= vel);
    return kTab[5 + i + 6 * (u & 4)];
}

// FUN_10021e20 (element = ei): the key gain (0 = no voice), counting the element's notes, the trigger condition and,
// for Sample elements, the zone lookup by the transposed key (rand draws). monoHasVoices: velocity gain 10000.
int PatchPlayer::elementGain(int ei, int tnote, int vel, uint32_t trig, bool monoHasVoices, const Zone*& zone,
                             const SampleData*& sd) {
    zone = nullptr;
    sd = nullptr;
    Elem& el = elems_[ei];
    if (!el.playable || el.p.mute) return 0;  // +0x41c (Mute, or Output Level below 0.0001)
    const auto& p = el.p;
    const int nk = std::clamp(tnote, 0, 127), nv = std::clamp(vel, 0, 127);
    const int gk = p.keyGain[nk];
    if (gk == 0) return 0;
    ++el.noteCounter;
    switch (p.triggerCond) {
        case 1: if (!(trig & 2)) return 0; break;
        case 2: if (trig & 2) return 0; break;
        case 3: if (trig & 4) return 0; break;
        case 4: if (!(trig & 4)) return 0; break;
        case 5: if (!(trig & 1)) return 0; break;
        case 6: if (trig & 1) return 0; break;
        case 7: if (!(el.noteCounter & 1)) return 0; break;
        case 8: if (el.noteCounter & 1) return 0; break;
        default: break;
    }
    if (!p.hasData) return 0;
    if (p.type == ElementType::Sample) {
        // ECO uses the instrument's second zone set when it has one (FUN_10019c20). No instrument (a piano layer
        // switched off, wave 0x80): +0xb90 stays 0, the rand draws still happen, then no voice.
        static const std::vector<Zone> kNoZones;
        const auto& zones = !el.inst ? kNoZones
                            : p.eco && !el.inst->ecoZones.empty() ? el.inst->ecoZones : el.inst->zones;
        const int fvel = p.receiveVelocity ? vel - 64 : 8;
        const int shift = quant(float(fvel) * p.formantVel + rng_.bi() * p.formantRand, 1.f) + p.formant;
        // +0xb98 = bit 9 of the first record's root word (FIX: record 0 even when it has no sample)
        const bool fixedRoot = !zones.empty() && ((zones.front().rootFlags >> 9) & 1);
        int zkey;
        if (!fixedRoot)
            zkey = quant(float(tnote - 60) * p.keytrack + float(p.semis + p.octaveSemis) + 60.5f, 1.f) - shift;
        else if (p.formant == 12)
            zkey = shift + tnote - 12;
        else
            zkey = shift + 48;
        zkey = std::clamp(zkey, 0, 127);
        // zone velocity modes 3/7 (@10021fc2): no voice unless part+0x970 (Piano Character - 0.375) >= 0.285
        if ((p.zoneVelMode == 3 || p.zoneVelMode == 7) && !(pianoChar_ >= 0.285f)) return 0;
        // piano layers 1 / 5 (main Model 1 / Model 2 layers): the zone velocity comes from FUN_1001c0f0 (no rand draw)
        const int zvel = p.zoneVelMode == 1 || p.zoneVelMode == 5
                             ? pianoZoneVel(el, vel)
                             : std::clamp(quant(float(fvel) * p.velScale + rng_.bi() * p.velRnd, 1.f) + p.velOffs, 1, 127);
        // FIX: the content manager's lookup (vtable +4 = FUN_1001b2a0) is a scan of the sorted records by their first
        // word (keyHi << 24 | velHi << 16 | keyLo << 8 | velLo, signed): the first record >= (key << 24 | vel << 16),
        // then within that record's keyHi the first one with velHi >= vel. No lower-bound test, and no end test (every
        // factory table ends with keyHi = velHi = 127). The found record's sample (+8) goes to vtable +8
        // (FUN_1001b2e0 -> FUN_10019e20): 0 for an index outside 1..records-1, i.e. no voice.
        if (zones.empty()) return 0;  // +0xb90 = 0: return 0 after the rand draws
        const int32_t want = int32_t((uint32_t(zkey) << 24) | (uint32_t(zvel) << 16));
        size_t zi = 0;
        while (zi < zones.size() && zones[zi].order() < want) ++zi;
        if (zi < zones.size()) {
            const int32_t w = int32_t((uint32_t(zones[zi].order()) & 0xff000000u) | (uint32_t(want) & 0xffffffu));
            while (zi < zones.size() && zones[zi].order() < w) ++zi;
        }
        if (zi >= zones.size()) return 0;
        zone = &zones[zi];
        if (zone->sample == 0 || zone->sample >= el.bank->sampleCount()) return 0;
        sd = el.bank->sample(zone->sample);
        if (!sd) return 0;
    }
    return gk * (monoHasVoices ? 10000 : int(p.velGain[nv]));
}

size_t PatchPlayer::preloadSamples(const std::function<bool()>& keepGoing) {
    size_t bytes = 0;
    for (auto& el : elems_) {
        if (el.p.type != ElementType::Sample || !el.p.hasData || !el.inst || !el.bank) continue;
        const auto& zones = el.p.eco && !el.inst->ecoZones.empty() ? el.inst->ecoZones : el.inst->zones;
        for (const auto& z : zones) {
            if (z.sample == 0 || z.sample >= el.bank->sampleCount()) continue;
            if (keepGoing && !keepGoing()) return bytes;
            if (const SampleData* sd = el.bank->sample(z.sample)) bytes += SampleBank::bytesOf(*sd);
        }
    }
    return bytes;
}

void PatchPlayer::collectSampleRefs(SampleRefs& out) const {
    for (const auto& el : elems_) {
        if (!el.bank || !el.inst) continue;
        for (const auto* zs : {&el.inst->zones, &el.inst->ecoZones})
            for (const auto& z : *zs)
                if (z.sample) out.zones.push_back({el.bank, z.sample});
    }
    for (const auto& v : voices_)
        if (v && v->lastSample()) out.voices.push_back(v->lastSample());
}

// FUN_10010130: remove list entry pos; its slot becomes the next free one.
void PatchPlayer::removeEntry(int pos) {
    const int8_t slot = list_[pos].slot;
    if (voices_[slot]) voices_[slot]->deactivate();
    --count_;
    for (int i = pos; i < count_; ++i) list_[i] = list_[i + 1];
    list_[count_].slot = slot;
}

// FUN_100101a0: the voice is released with its Voice Mute Fadeout and stamped with the part note counter.
void PatchPlayer::stealEntry(int pos) {
    list_[pos].held = 0;
    Voice* v = voices_[list_[pos].slot].get();
    if (v) {
        v->steal();
        v->stamp = int(partNotes_);
    }
    ++partNotes_;
}

// FUN_100102b0: which list position (and slot) a new voice of element ei gets. Quiet voices (released or fading, amp
// env level < 0.03) are re-released with the mute fadeout and replaced; at the element's Max Voices or the patch
// polyphony (+6) the oldest voice of the lowest priority class is stolen.
int PatchPlayer::allocate(int ei) {
    if (count_ == 0) {
        count_ = 1;
        return 0;
    }
    float minAll = 0.03f, minElem = 0.03f;
    int quiet = -1, quietElem = -1, first = -1, nElem = 0, minPrioAll = 99, minPrioElem = 99;
    int prio[64];
    for (int i = 0; i < count_; ++i) {
        const Entry& en = list_[i];
        const Voice* v = voices_[en.slot].get();
        const int pr = elems_[en.elem].p.priority;
        const int st = v ? v->endState() : 1;
        const float amp = v ? v->ampLevel() : 0.f;
        prio[i] = pr;
        if (st == 2) {
            prio[i] = pr + 3;
        } else if (en.elem == ei) {
            if (first < 0) first = i;
            ++nElem;
            if (st == 1 || en.held == 0) {
                if (amp < minAll) {
                    minAll = amp;
                    quiet = i;
                }
                if (amp < minElem) {
                    quietElem = i;
                    minElem = amp;
                }
            } else {
                prio[i] = (en.held != -1) + 1 + pr;
            }
            minPrioElem = std::min(minPrioElem, prio[i]);
        } else if (st == 1 || en.held == 0) {
            if (amp < minAll) {
                minAll = amp;
                quiet = i;
            }
        } else {
            prio[i] = (en.held != -1) + 1 + pr;
        }
        minPrioAll = std::min(minPrioAll, prio[i]);
    }
    auto insertNew = [&]() {  // the free slot at position count_, moved to the end of element ei's entries
        int pos = count_;
        const int8_t slot = list_[pos].slot;
        while (pos > 0 && list_[pos - 1].elem > ei) {
            list_[pos] = list_[pos - 1];
            --pos;
        }
        ++count_;
        list_[pos].slot = slot;
        return pos;
    };
    int lo, hi, minPrio;
    if (nElem < elems_[ei].p.maxVoices) {
        if (quiet < 0 && count_ < poly_) return insertNew();
        lo = 0;
        hi = count_;
        minPrio = minPrioAll;
    } else {
        if (quietElem >= 0) quiet = quietElem;
        lo = first;
        hi = first + nElem;
        minPrio = minPrioElem;
    }
    int pick = quiet;
    if (pick < 0) {
        int best = int(partNotes_);
        for (int i = lo; i < hi; ++i) {
            const int st = voices_[list_[i].slot] ? voices_[list_[i].slot]->stamp : 0;
            if (prio[i] == minPrio && st < best) {
                pick = i;
                best = st;
            }
        }
    }
    if (pick >= 0 && count_ < poly_ + 6 && count_ < 64) {
        stealEntry(pick);
        return insertNew();
    }
    if (pick < 0) pick = 0;
    // reuse the picked entry's slot: take it out and re-insert it after element ei's entries
    const int8_t slot = list_[pick].slot;
    int pos = count_ - 1;
    for (int i = pick; i < pos; ++i) list_[i] = list_[i + 1];
    while (pos > 0 && list_[pos - 1].elem > ei) {
        list_[pos] = list_[pos - 1];
        --pos;
    }
    list_[pos].slot = slot;
    return pos;
}

// FUN_10010670: Mute Condition of a new note's element (1 same note, 2 other elements, 3 whole patch, >= 4 group).
void PatchPlayer::muteCondition(int mode, int ei, int note) {
    for (int i = 0; i < count_; ++i) {
        const Entry& en = list_[i];
        bool hit;
        if (mode == 1) hit = en.note == note;
        else if (mode == 2) hit = en.elem != ei;
        else if (mode == 3) hit = true;
        else hit = en.group == mode && (elems_[en.elem].p.monoMode != 0 || en.note != note);
        if (hit) stealEntry(i);
    }
}

// FUN_10010200: the held note a mono element plays (mode 1 lowest, 2 highest, 3 first, 4 last), skipping notes
// outside its key range; -1 if none.
int PatchPlayer::pickMono(int ei, int mode) const {
    int n = heldCount_;
    const int8_t* p;
    int dir;
    if (mode == 2) p = &heldSorted_[n - 1], dir = -1;
    else if (mode == 3) p = &heldOrder_[0], dir = 1;
    else if (mode == 4) p = &heldOrder_[n - 1], dir = -1;
    else p = &heldSorted_[0], dir = 1;
    const auto& kg = elems_[ei].p.keyGain;
    for (;;) {
        if (*p >= 0 && kg[*p] * partKeyGain_[*p] != 0) return *p;  // element * part key gain (+0x97c)
        if (--n == 0) return -1;
        p += dir;
    }
}

// FUN_10014910, the part side: the list entry, the patch LFO retrigger, then the voice's note-on.
void PatchPlayer::startVoice(int note, int cents, int vel, float gain, int ei, int pos, int unison, const Zone* zone,
                             const SampleData* sd) {
    Elem& el = elems_[ei];
    const auto& p = el.p;
    Entry& en = list_[pos];
    en.elem = int8_t(ei);
    en.group = p.muteCond;
    en.held = int8_t(note);
    en.note = int8_t(note);
    // FIX: the patch LFO1/2 retrigger runs per voice start (@10014990): sync 1 when < 2 notes are held, 2/3 always
    auto retrig = [&](Lfo& l, const LfoParams& lp) {
        if ((lp.sync == 1 && heldCount_ < 2) || lp.sync == 2 || lp.sync == 3) {
            const float ph = lp.phase <= 0.9f ? float(double(lp.phase) * double(1.1111f)) : rng_.uni();
            l.init(ph, lp.wave, rng_);
        }
    };
    retrig(lfo1_, lfo1P_);
    retrig(lfo2_, lfo2P_);
    const int vi = en.slot;
    if (!voices_[vi] || voiceType_[vi] != p.type) {
        auto nv = makeVoice(p.type);
        if (nv && voices_[vi]) nv->inheritSlot(*voices_[vi]);  // the original's slot keeps its words
        voices_[vi] = std::move(nv);
        voiceType_[vi] = p.type;
    }
    VoiceStart vs;
    vs.unisonIndex = unison;
    vs.note = note;
    vs.transpose = transpose_;
    vs.bpm = float(bpmD_);  // part+0xd90 (double) as float
    vs.bps = bpsD_;         // part+0xd88
    vs.lastGlide = &el.lastGlide;
    // Fingered glide (@10014d71): another held note inside the part key range
    vs.otherHeld = false;
    for (int i = 0; i < heldCount_; ++i)
        if (partKeyGain_[heldOrder_[i] & 127] != 0 && heldOrder_[i] != en.held) vs.otherHeld = true;
    vs.velocity = vel;
    vs.cents = cents;
    vs.tune = tuneCents(note, &vs.scale2);  // FUN_10010870(note before the transpose, voice+0x310)
    vs.sustainWord = float(sustain_);
    vs.softWord = softPedal_;
    vs.altCounter = el.noteCounter;  // FIX: elem+0xb6c (counted in FUN_10021e20), not the part's counter
    vs.noteGain = gain;
    vs.zone = zone;
    vs.sample = sd;
    vs.monoLegato = heldCount_ >= 2 && p.monoMode != 0;
    for (int i = 0; i < count_; ++i) {  // drawbar percussion trigger (@10015a9a): the other held drawbar voices
        float lv;
        const Voice* o = voices_[list_[i].slot].get();
        if (i != pos && list_[i].held != 0 && o && o->percLevel(lv) && vs.drawbarOther < lv) vs.drawbarOther = lv;
    }
    voices_[vi]->start(p, c_, vs, inputs_, rng_);
    voices_[vi]->stamp = int(partNotes_);
}

void PatchPlayer::legato(int pos, int note, int cents, int vel, int mode) {  // FUN_10016820 (list bytes + the voice)
    list_[pos].held = int8_t(note);
    list_[pos].note = int8_t(note);
    Voice* v = voices_[list_[pos].slot].get();
    if (!v) return;
    v->legato(note, transpose_, vel, mode, elems_[list_[pos].elem].p.patchGlide, rng_, cents, float(sustain_),
              softPedal_, tuneCents(note, nullptr));
    if (mode == 0) v->stamp = int(partNotes_);
}

// FUN_10017880, note-on (0x90, velocity > 0). fromArp: an arpeggiator event (part gain 1e8, byte 3 = semitones).
void PatchPlayer::noteOnEvent(int note, int velocity, int detune, bool fromArp) {
    // held lists: part+0x20 in order (no duplicates), part+0xa0 sorted
    {
        bool known = false;
        for (int i = 0; i < heldCount_; ++i) known |= heldOrder_[i] == note;
        if (!known) {
            heldOrder_[heldCount_] = int8_t(note);
            int i = heldCount_;
            while (i != 0 && note < heldSorted_[i - 1]) {
                heldSorted_[i] = heldSorted_[i - 1];
                --i;
            }
            heldSorted_[i] = int8_t(note);
            ++heldCount_;
        }
    }
    int cents, pg;
    if (!fromArp) {
        noteDetune_[note & 127] = int8_t(detune);  // part+0x120 + note
        cents = detune;
        pg = int(partVelGain_[velocity & 127]) * int(partKeyGain_[note & 127]);
    } else {
        cents = detune * 100;
        pg = 100000000;
    }
    if (maxVel_ < velocity) maxVel_ = velocity;
    notePitch_ = float(double(heldSorted_[heldCount_ - 1] + transpose_) * double(kSemitone) +
                       double(inputs_.pitchOfs));
    if (pg == 0 || partMute_) return;
    const float pgain = float(double(pg) * double(1e-8f));
    // trigger-condition flags for this note (bit 0 patch-note parity, 1 mod wheel up, 2 sustain held)
    const uint32_t trig = (partNotes_++ & 1u) | (modWheelRaw_ > 0.5f ? 2u : 0u) | (sustain_ ? 4u : 0u);
    for (int ei = 0; ei < int(elems_.size()); ++ei) {
        Elem& el = elems_[ei];
        if (!el.playable) continue;
        const auto& p = el.p;
        bool monoHas = false;
        if (p.monoMode != 0)
            for (int i = 0; i < count_; ++i) monoHas |= list_[i].elem == ei;
        const Zone* zone;
        const SampleData* sd;
        const int g = elementGain(ei, note + transpose_, velocity, trig, monoHas, zone, sd);
        if (g == 0) continue;
        // @100181c8: float(g * pgain * 1e-8f), pgain = float(part key * velocity gain * 1e-8f)
        const float gain = float(double(g) * double(pgain) * double(1e-8f));
        if (p.muteCond != 0) muteCondition(p.muteCond, ei, note);
        const int mono = p.monoMode;
        if (mono == 0) {
            // legato pedal (CC68): the element's held voices glide to the new note (with this note's byte-3 detune
            // from part+0x120), then a new voice starts as usual
            if (legatoPedal_ != 0 && heldCount_ > 1)
                for (int i = count_ - 1; i >= 0; --i)
                    if (list_[i].elem == ei && list_[i].held != 0)
                        legato(i, note, noteDetune_[note & 127], velocity, p.legatoRetrig);
            if (modReset_) {  // part+0x978 == 1 (a mod row's source / dest changed): +0xb80 = the note counter
                modReset_ = false;
                resumeFrom_ = partNotes_;
            }
            if (p.envResume) {  // Envelope Mode "Analog (Resume)": the same note's voice is retriggered in place
                int found = -1;
                for (int i = 0; i < count_; ++i) {
                    const Voice* vv = voices_[list_[i].slot].get();
                    if (list_[i].elem == ei && list_[i].note == note && vv && vv->stamp >= int(resumeFrom_))
                        found = i;  // stamp >= part+0xb80
                }
                if (found >= 0) {
                    legato(found, note, cents, velocity, 0);
                    continue;
                }
            }
            const int pos = allocate(ei);
            startVoice(note, cents, velocity, gain, ei, pos, 0, zone, sd);
            continue;
        }
        if (mono == 3) {
            if (heldCount_ >= 2) continue;
        } else if (mono != 4 && pickMono(ei, mono) != note) {
            continue;
        }
        // mono note (stack of Mono Unison Voices + 1)
        const int N = p.unisonVoices;
        const float saved = el.lastGlide;
        int remaining = N + 1;
        uint32_t mask = 0;
        if (p.legatoRetrig == 0 || modReset_) {  // Start New Voice, or part+0x978 >= 1 (cleared)
            modReset_ = false;
            for (int i = 0; i < count_; ++i)
                if (list_[i].elem == ei) stealEntry(i);
        } else {
            for (int i = count_ - 1; i >= 0; --i) {
                if (list_[i].elem != ei) continue;
                const Voice* v = voices_[list_[i].slot].get();
                int mode;
                if (list_[i].held == 0) {
                    if (!p.envResume) {
                        stealEntry(i);
                        continue;
                    }
                    mode = 0;
                } else {
                    mode = p.legatoRetrig;
                }
                if (N != 0 && v) mask |= 1u << ((v->unison - 1) & 31);
                el.lastGlide = saved;
                legato(i, note, cents, velocity, mode);
                if (--remaining == 0) break;
            }
        }
        for (; remaining > 0; --remaining) {
            int idx = 0;
            if (N != 0) {
                while (mask & (1u << idx)) ++idx;
                mask |= 1u << idx;
                ++idx;
            }
            el.lastGlide = saved;
            const int pos = allocate(ei);
            startVoice(note, cents, velocity, gain, ei, pos, idx, zone, sd);
        }
    }
}

// FUN_10017880, note-off (0x80, or 0x90 with velocity 0). Note bytes 0x80..0xfe stand for the note -byte without a
// held-list update; 0xff (-1) is the sustain pedal's release of the sustained entries.
void PatchPlayer::noteOffEvent(uint8_t noteByte) {
    int8_t nb = int8_t(noteByte);
    if (nb < -1) {
        nb = int8_t(-nb);
    } else {
        int io = -1, is = -1;
        for (int i = 0; i < heldCount_; ++i) {
            if (heldOrder_[i] == nb) io = i;
            if (heldSorted_[i] == nb) is = i;
        }
        if (io >= 0) {
            --heldCount_;
            for (int i = io; i < heldCount_; ++i) heldOrder_[i] = heldOrder_[i + 1];
            for (int i = is; i < heldCount_; ++i) heldSorted_[i] = heldSorted_[i + 1];
        }
        if (heldCount_ == 0) {
            if (sustain_ == 0) notePitch_ = -10.f;
        } else {
            notePitch_ = float(double(heldSorted_[heldCount_ - 1] + transpose_) * double(kSemitone) +
                               double(inputs_.pitchOfs));
        }
    }
    // with the arpeggiator on (and not in Ctrl Seq mode) the pedal does not hold notes; the mono return to a held
    // note is skipped while it is on
    const bool arpOn = arp_->on() && arp_->mode() != 7, arpOnAny = arp_->on();
    for (int i = 0; i < count_; ++i) {
        if (list_[i].held != nb) continue;
        if (sustain_ != 0 && !arpOn) {  // pedal down: held as -1 until the pedal goes up
            list_[i].held = -1;
            continue;
        }
        const int ei = list_[i].elem;
        const auto& p = elems_[ei].p;
        Voice* v = voices_[list_[i].slot].get();
        int other = -1;
        if (p.monoMode != 0 && heldCount_ != 0 && !arpOnAny) other = pickMono(ei, p.monoMode);
        if (other < 0) {
            list_[i].held = 0;
            if (v) {
                v->setReleaseKey(nb);
                v->release();
            }
            continue;
        }
        // mono: back to a held note with the released voice's velocity and that note's detune byte
        const int vel = int(double(v ? v->velWord() : 0.f) * 63.0 + 64.5);
        const int cents = noteDetune_[other & 127];
        if (p.releaseRetrig == 0) {
            const Zone* zone;
            const SampleData* sd;
            const int g = elementGain(ei, other + transpose_, vel, 0, true, zone, sd);
            if (g != 0) {
                ++heldCount_;
                const int uni = v ? v->unison : 0;
                const int pos = allocate(ei);
                startVoice(other, cents, vel, float(double(g) * double(1e-8f)), ei, pos, uni, zone, sd);
                --heldCount_;
            }
            stealEntry(i);  // original quirk: by position, after the allocation may have moved entries
        } else {
            legato(i, other, cents, vel, p.releaseRetrig);
        }
    }
}

// An element of the part render loop (FUN_100139c0): inserts get +1e-6 at frame 0, are processed (unless muted) and
// route to the main / send buses when they have an Aux Out (after the last voice only once the part is not silent).
void PatchPlayer::processInsert(int ei, bool beforeLastVoice, int n, bool& fxSilent, bool& partSilent, float* out,
                                float* const* sendBus, bool* const* sendUsed, int ofs) {
    Elem& el = elems_[ei];
    if (!el.isEffect || el.orig == 0) return;  // the loop starts at element 1
    bus_[0] += kBias;
    bus_[1] += kBias;
    if (el.fx && !el.muted) {
        const bool in = fxSilent;
        el.fx->processGated(bus_.data(), n, fxSilent);
        ringOut(el.ring, in, fxSilent, bus_.data(), n);
    }
    if (!beforeLastVoice && !fxSilent) partSilent = false;
    if (el.auxOut == 0 || (!beforeLastVoice && partSilent)) return;
    const float R = el.outGain * el.pan, L = el.outGain - R;
    if (!eng_) {
        addScaled(out, bus_.data(), n, L * outL_, R * outR_);  // TODO: aux outputs (multi-out) -> main
    } else if (partOutput_ != 0) {  // the part's Output; Aux Out > 2 picks the output (Aux Out - 2) above it
        int o = partOutput_;
        if (el.auxOut > 2) o = std::min(o - 2 + el.auxOut, eng_->outputs);
        addOut(o, ofs, bus_.data(), n, L * outL_, R * outR_);
    }
    for (int k = 0; k < 4; ++k) {
        const float s = patchSend_[k] * el.send[k];
        if (s > 0.0001f) {
            (eng_ ? addScaledX87 : addScaled)(sendBus[k], bus_.data(), n, s * outL_, s * outR_);
            *sendUsed[k] = true;
        }
    }
    if (el.auxOut > 1) {
        std::fill(bus_.begin(), bus_.end(), 0.f);
        bus_[0] = bus_[1] = kBias;
        fxSilent = true;
    }
}

// The part's main output (FUN_100139c0 @10013... -> FUN_1000fa30, Output != 0): the bus times the part gains,
// ramped linearly from the last call's gains (part+0xb84 / +0xb88) by (target - last) * 0.001 per frame (1 / n for
// calls of >= 1000 frames), snapping to the target within 0.003. The x87 keeps the gains in registers with the
// float stores of the 4-frame unrolled loop mirrored below.
void PatchPlayer::mainOut(float* out, int n) {
    const float L = outL_, R = outR_;
    const float* in = bus_.data();
    if (L == rampL_ && R == rampR_) {  // FUN_1000f660
        (eng_ ? addScaledX87 : addScaled)(out, in, n, L, R);
        return;
    }
    const double step = n < 1000 ? double(0.001f) : 1.0 / double(n);
    const float sL = float((double(L) - double(rampL_)) * step);
    const float sR = float((double(R) - double(rampR_)) * step);
    double gL = rampL_, gR = rampR_;
    float gRs = rampR_;  // [esp+0x1c]: the stored copy of the right gain
    auto mac = [](float* o, double g, float x) { *o = float(g * double(x) + double(*o)); };
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float* o = out + 2 * i;
        const float* x = in + 2 * i;
        mac(o, gL, x[0]);
        mac(o + 1, gR, x[1]);
        gL = gL + double(sL);
        gR = double(gRs) + double(sR);
        mac(o + 2, gL, x[2]);
        mac(o + 3, gR, x[3]);
        const float gL2 = float(double(sL) + gL);
        gR = gR + double(sR);
        mac(o + 4, gL2, x[4]);
        mac(o + 5, gR, x[5]);
        const float gL3 = float(double(sL) + double(gL2));
        const float gR3 = float(gR + double(sR));
        mac(o + 6, gL3, x[6]);
        mac(o + 7, gR3, x[7]);
        gL = double(sL) + double(gL3);
        gR = double(gR3) + double(sR);
        gRs = float(gR);
    }
    for (; i < n; ++i) {
        mac(out + 2 * i, gL, in[2 * i]);
        mac(out + 2 * i + 1, gR, in[2 * i + 1]);
        gL = double(sL) + gL;
        gR = gR + double(sR);
        gRs = float(gR);
    }
    rampL_ = std::fabs(gL - double(L)) < double(0.003f) ? L : float(gL);
    rampR_ = std::fabs(double(gRs) - double(R)) < double(0.003f) ? R : gRs;
}

// One part render call (FUN_100139c0) + the patch's own send FX (FUN_1000f130). Adds into `out`; sends to the global
// send FX go to globalBus_ at frame offset sendOfs. Engine mode (eng_): the outputs, global buses and the per-slot send
// routing of the original (a slot's sends go to the part's own send FX when FX mode (0x3e) is on and the slot has an
// effect object, else to the global bus; part Output 0 = no main / aux output), and FUN_1000f130 for the own send FX.
// Returns FUN_100139c0's result: true = the part was silent.
bool PatchPlayer::renderCall(float* out, int n, double songPos, int sendOfs) {
    dbgClock_ += n;
    for (auto& s : sendBus_) s.assign(size_t(n) * 2, 0.f);
    bool sendUsed[4] = {};  // part+0x1138: something was mixed into send bus k this call
    bool* used = ownSends_ ? sendUsed : globalUsed_;
    float* sendBus[4];
    bool* usedK[4];
    for (int k = 0; k < 4; ++k) {
        if (!eng_) {
            sendBus[k] = ownSends_ ? sendBus_[k].data() : globalBus_[k].data() + size_t(sendOfs) * 2;
            usedK[k] = &used[k];
        } else {
            const bool own = ownSends_ && sendFx_[k].fx;
            sendBus[k] = own ? sendBus_[k].data() : eng_->gbus[k] + size_t(sendOfs) * 2;
            usedK[k] = own ? &sendUsed[k] : &eng_->gused[k];
        }
    }
    if (asleep_) {
        rampReset_ = true;  // part+0x18
        return true;
    }
    if (eng_)
        for (int p = 0; p < eng_->outputs; ++p) engOut_[p] = eng_->out[p] + size_t(sendOfs) * 2;
    if (rampReset_) {  // the output ramp starts at the current gains after a reset / sleep
        rampReset_ = false;
        rampL_ = outL_;
        rampR_ = outR_;
    }
    // controller words: +0xda4 = +0xda0; aftertouch +0xdac += (+0xda8 - +0xdac) * min(n * 10/sr, 1)
    inputs_.modWheel = modWheelRaw_;
    {
        double f = double(n) * double(float(10.0 / double(c_.sr)));  // part+0x4cc
        if (f > 1.0) f = 1.0;
        inputs_.aftertouch = float((double(atRaw_) - double(inputs_.aftertouch)) * f + double(inputs_.aftertouch));
    }
    for (auto& el : elems_)
        if (el.fx) el.fx->setSongPos(songPos);  // process() args 3/4
    for (auto& s : sendFx_)
        if (s.fx && (ownSends_ || eng_)) s.fx->setSongPos(songPos);
    bus_.assign(size_t(n) * 2, 0.f);
    bus_[0] = bus_[1] = kBias;
    // FIX: the patch LFOs tick once per 32 frames of the call: pairs (LFO1, LFO2) for i < n >> 4, step 2, then
    // the last pair is repeated; a call < 16 frames leaves the array as it was. Voices read one pair per control
    // update from the start of the array.
    const int k = n >> 4;
    if (k > 0 && k < n) {
        if (int(lfoArr_.size()) < k + 4) {
            lfoArr_.resize(size_t(k) + 4, 0.f);
            inputs_.lfoArr = lfoArr_.data();
        }
        int i = 0;
        for (; i < k; i += 2) {
            lfoArr_[i] = lfo1_.tick(lfo1P_.rate, rng_);
            lfoArr_[i + 1] = lfo2_.tick(lfo2P_.rate, rng_);
        }
        lfoArr_[i] = lfoArr_[i - 2];
        lfoArr_[i + 1] = lfoArr_[i - 1];
    }
    // The voices in list order (sorted by element), each after the inserts of the elements before its own; the
    // insert "silent" flag starts set, a voice clears it, an Aux Out that clears the bus sets it again.
    bool fxSilent = true, partSilent = true;
    int next = 0;
    if (count_ > 0) {
        partSilent = false;
        for (int i = 0; i < count_; ++i) {
            const int ve = list_[i].elem;
            for (; next < ve; ++next)
                processInsert(next, true, n, fxSilent, partSilent, out, sendBus, usedK, sendOfs);
            if (Voice* v = voices_[list_[i].slot].get()) v->render(bus_.data(), n);
            fxSilent = false;
        }
    }
    for (; next < int(elems_.size()); ++next)
        processInsert(next, false, n, fxSilent, partSilent, out, sendBus, usedK, sendOfs);
    if (!partSilent) {
        // remaining bus: sends (to the patch's own send FX, or the global ones) + main
        for (int k2 = 0; k2 < 4; ++k2)
            if (patchSend_[k2] > 0.0001f) {
                (eng_ ? addScaledX87 : addScaled)(sendBus[k2], bus_.data(), n, outL_ * patchSend_[k2],
                                                  outR_ * patchSend_[k2]);
                *usedK[k2] = true;
            }
        if (!eng_) {
            mainOut(out, n);
        } else if (partOutput_ != 0) {  // FUN_1000fa30 to the part's Output (0 = none: the ramp stays)
            mainOut(engOut_[partOutput_ - 1], n);
            eng_->outUsed[2 * (partOutput_ - 1)] = eng_->outUsed[2 * (partOutput_ - 1) + 1] = true;
        }
    }
    if (eng_) {
        if (sendFxRender(n, songPos, sendOfs, sendUsed)) partSilent = false;
    } else if (ownSends_) {
        // the patch's own send FX, FX4 down to FX1 (each may feed FX1), part of the part render
        for (int k2 = 3; k2 >= 0; --k2) {
            SendFx& s = sendFx_[k2];
            if (!s.fx) continue;
            // FUN_1000f130: silent = nothing was sent to this bus; +1e-6 at frame 0 before the effect; the return
            // is used only if the effect cleared the flag
            bool sil = !sendUsed[k2];
            sendBus_[k2][0] += kBias;
            sendBus_[k2][1] += kBias;
            s.fx->processGated(sendBus_[k2].data(), n, sil);
            ringOut(s.ring, !sendUsed[k2], sil, sendBus_[k2].data(), n);
            if (sil) continue;
            partSilent = false;
            if (k2 != 0 && s.send1 > 1e-8f) {
                addScaled(sendBus_[0].data(), sendBus_[k2].data(), n, s.send1, s.send1);
                sendUsed[0] = true;
            }
            if (s.routed) {
                const float R = s.level * s.pan, L = s.level - R;
                addScaled(out, sendBus_[k2].data(), n, L, R);
            }
        }
    }
    // FIX: the voice-end tests over the voice list; a removal (FUN_10010130) shifts the list, so the entry after
    // a removed one is not tested until the next call. 1) +0x534 != 0 and amp level < 1e-4; 2) ended one-shots.
    for (int i = 0; i < count_; ++i) {
        Voice* v = voices_[list_[i].slot].get();
        if (v) v->syncModRow();  // FUN_10010c50 when +0x518 != elem+0x5f0
        if (!v || v->ampEnded()) removeEntry(i);
    }
    for (int i = 0; i < count_; ++i) {
        Voice* v = voices_[list_[i].slot].get();
        if (v && v->oneShotEnded()) removeEntry(i);
    }
    // FIX: a silent call (no voices, inserts and send FX silent) puts the part to sleep until the next MIDI
    // event (part+0x1140; not while the arp runs, arp +0x186): no LFO ticks, no effect calls. See setRingOut.
    asleep_ = partSilent && !arp_->running();  // @10014879: not while the arp runs
    return partSilent;
}

// Ring-out mode (setRingOut; not in the original): after an effect's gated process, keep it "not silent" for
// kRingSeconds after its input was used or its output was audible on either channel (the original's test,
// 2|x| > 1e-5, on L or R), so the part does not sleep and the effect's output keeps reaching the outputs.
void PatchPlayer::ringOut(int& ring, bool inSilent, bool& outSilent, const float* lr, int frames) const {
    if (!ringOut_) return;
    bool audible = !inSilent;
    for (int i = 0; i < 2 * frames && !audible; ++i) audible = double(std::fabs(lr[i])) * 2.0 > double(1e-05f);
    if (audible) {
        ring = int(double(c_.sr) * kRingSeconds);
        outSilent = false;
    } else if (ring > 0) {
        ring -= frames;
        outSilent = false;
    }
}

// Engine output pair `pair` (1-based) at frame offset ofs: += src * gains (FUN_1000f660 with add), marks it used.
void PatchPlayer::addOut(int pair, int ofs, const float* src, int frames, float gl, float gr) {
    addScaledX87(eng_->out[pair - 1] + size_t(ofs) * 2, src, frames, gl, gr);
    eng_->outUsed[2 * (pair - 1)] = eng_->outUsed[2 * (pair - 1) + 1] = true;
}

// FUN_1000f130 (engine mode): the four send FX slots FX4 .. FX1. A part (ofs >= 0) processes its own send FX on its
// own buses (filled this call); the global part (ofs < 0) processes the global send FX on the content's global buses
// (the whole block). Unmuted slots with an effect get +1e-6 at frame 0 and the gated process ("silent" = nothing was
// sent to the bus); an audible result feeds the global FX1 bus (FX2..FX4 with Send 1 > 1e-8: content bus 1, also for
// a part's own send FX, at the call's frame offset) and returns to output Aux Out (level 4 v, linear pan). Every bus is
// cleared after.
bool PatchPlayer::sendFxRender(int n, double songPos, int ofs, bool* ownUsed) {
    const bool global = ofs < 0;
    if (global) ofs = 0;
    bool any = false;
    for (int k = 3; k >= 0; --k) {
        float* bus = global ? eng_->gbus[k] : sendBus_[k].data();
        bool& used = global ? eng_->gused[k] : ownUsed[k];
        bool silent = !used;
        SendFx& s = sendFx_[k];
        if (!s.muted && s.fx) {
            bus[0] += kBias;
            bus[1] += kBias;
            s.fx->setSongPos(songPos);
            s.fx->processGated(bus, n, silent);
            ringOut(s.ring, !used, silent, bus, n);
            if (!silent) {
                any = true;
                if (k != 0 && s.send1 > 1e-8f) {
                    float* b1 = eng_->gbus[0] + size_t(ofs) * 2;
                    const double g = s.send1;
                    if (!eng_->gused[0])
                        for (int i = 0; i < 2 * n; ++i) b1[i] = float(g * double(bus[i]));
                    else
                        for (int i = 0; i < 2 * n; ++i) b1[i] = float(g * double(bus[i]) + double(b1[i]));
                    eng_->gused[0] = true;
                }
                if (s.auxOut != 0) {
                    const float lv = s.level;  // raw Output Level * 4
                    if (lv > 0.f) {
                        const float R = lv * s.pan, L = lv - R;
                        float* o = eng_->out[s.auxOut - 1] + size_t(ofs) * 2;
                        for (int i = 0; i < n; ++i) {
                            o[2 * i] = float(double(L) * double(bus[2 * i]) + double(o[2 * i]));
                            o[2 * i + 1] = float(double(R) * double(bus[2 * i + 1]) + double(o[2 * i + 1]));
                        }
                    }
                    eng_->outUsed[2 * (s.auxOut - 1)] = eng_->outUsed[2 * (s.auxOut - 1) + 1] = true;
                }
            }
        }
        std::fill(bus, bus + 2 * size_t(n), 0.f);
        used = false;
    }
    return any;
}

// A new part's patch parameters (part ctor FUN_10018b80: part+0x4d0 + 4 idx).
const ParamArray& PatchPlayer::defaultParams() {
    static const ParamArray q = [] {
        ParamArray p{};
        auto b = [](uint32_t u) { return std::bit_cast<float>(u); };
        p[0x3b] = b(0x3f350481);
        p[0x3c] = p[0x3a] = p[0x39] = p[0x01] = p[0x37] = p[0x38] = 0.5f;
        p[0x3d] = b(0x3d4ccccd);
        p[0x03] = b(0x3ecccccd);
        p[0x32] = p[0x35] = 1.f;
        p[0x0e] = p[0x0f] = p[0x67] = p[0x69] = p[0x6b] = p[0x6d] = 0.5f;
        p[0x4d] = p[0x51] = 1.f;
        p[0x4e] = p[0x52] = p[0x53] = 0.5f;
        p[0x49] = p[0x4a] = b(0x3e19999a);
        p[0x17] = b(0x3f4ccccd);
        p[0x18] = p[0x1c] = 0.5f;
        p[0x1d] = b(0x3ed47ae1);
        p[0x1e] = 1.f;
        p[0x55] = b(0x3e23d70a);
        p[0x57] = b(0x3f2b851f);
        p[0x59] = b(0x3e8f5c29);
        p[0x5b] = 1.f;
        p[0x46] = b(0x3e3851ec);
        p[0x97] = 1.f;
        for (int i = 0x7e; i <= 0x87; ++i) p[size_t(i)] = 0.5f;
        p[0x94] = 0.5f;
        for (int i = 0xac; i < 0xfc; ++i) p[size_t(i)] = 0.5f;
        return p;
    }();
    return q;
}

// A new element's raw parameters as far as a send FX slot uses them (element ctor FUN_10025050): muted, type 128
// (raw 1.0), knobs 0.5 0.5 1 0 1 0 0 0 0.5 1 0 0, level / pan 0.5, no quick-knob destinations.
PatchPlayer::SendFx PatchPlayer::freshSendFx() {
    SendFx s;
    auto& r = s.raw;
    r[0x00] = 1.f;
    r[0x02] = r[0x03] = 1.f;
    r[0x04] = r[0x05] = std::bit_cast<float>(0x3f2b851fu);
    r[0x07] = r[0x09] = r[0x0b] = r[0x0d] = r[0x0e] = r[0x0f] = 0.5f;
    r[0x13] = 1.f;
    static constexpr float kKnob[12] = {0.5f, 0.5f, 1.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.5f, 1.f, 0.f, 0.f};
    for (int i = 0; i < 12; ++i) r[size_t(0x19 + i)] = kKnob[i];
    r[0x8b] = r[0x8c] = 0.5f;
    s.muted = true;
    s.level = 2.f;
    s.pan = 0.5f;
    return s;
}

// FUN_10016f70, the send FX records (engine mode). A record with flag 2 of a patch with elements goes to the global
// part's slot when that slot is empty (raw type >= 0.999), and the part's own slot is then muted and emptied
// (setter 0 / 0x12 / 0x13 = 1); when the global slot holds an effect, a record named "Long Reverb" / "GM Reverb"
// (slot 1), "Short Reverb" (slot 2) or "GM Chorus" (slot 3) is skipped, any other goes to the part's own slot. Flag 1:
// the own slot. Each record is set through the setter (param 1 = 0), index 0..255.
void PatchPlayer::loadSendFxEngine(const Patch& patch) {
    for (auto& s : sendFx_) s = freshSendFx();
    size_t rec = 0;
    for (int k = 0; k < 4; ++k) {
        const uint8_t flag = patch.sendFxFlags[size_t(k)];
        if (!flag || rec >= patch.sendFx.size()) continue;
        const PatchRecord& r = patch.sendFx[rec++];
        PatchPlayer* target = this;
        if (globalPart_ && flag == 2 && !patch.elements.empty()) {
            target = globalPart_;
            if (globalPart_->sendFx_[size_t(k)].raw[0x13] <= 0.999f) {
                const bool standard = (k == 0 && (r.name == "Long Reverb" || r.name == "GM Reverb")) ||
                                 (k == 1 && r.name == "Short Reverb") || (k == 2 && r.name == "GM Chorus");
                if (standard) continue;
                target = this;
            }
        }
        for (int idx = 0; idx < 0x100; ++idx) target->setParam(0x41 + k, idx, idx == 1 ? 0.f : r.params[size_t(idx)]);
        if (target != this) {
            setParam(0x41 + k, 0x00, 1.f);
            setParam(0x41 + k, 0x12, 1.f);
            setParam(0x41 + k, 0x13, 1.f);
        }
    }
}

// FUN_100221f0 on a send FX element (slot 0..3 = setter element 0x41..0x44): the cases an effect element uses.
void PatchPlayer::setSendFxParam(int slot, int idx, float v) {
    SendFx& s = sendFx_[slot];
    s.raw[idx] = v;
    switch (idx) {
        case 0x00: s.muted = v > 0.5f; break;  // case 0 (type 0: the level does not mute)
        case 0x13: {                           // case 0x6b: a new effect only when the type changes
            const int type = quant(v, 128.9f);
            if (type == s.fxType) break;
            s.fxType = type;
            s.fx = createEffect(type);
            if (s.fx) {
                s.fx->setRandom(&rng_);
                s.fx->setTempo(bpm_, bps_);
                s.fx->setPitchSource(&notePitch_);
                s.fx->setSampleRate(c_.sr);
                const ParamArray q = elementKnobParams(s.raw, patch_.params);
                for (int i = 0; i < 12; ++i) s.fx->setParam(i, q[0x19 + i]);
            }
            break;
        }
        case 0x14:
            s.auxOut = std::min(quant(v, 32.9f), eng_ ? eng_->outputs : 1);
            s.routed = s.auxOut != 0;
            break;
        case 0x15: s.send1 = v * v * 4.f; break;
        case 0x8b: s.level = v * 4.f; break;
        case 0x8c: s.pan = v; break;
        case 0x91: s.quick[0] = quant(v, 7.9f); break;
        case 0x92: s.quick[1] = quant(v, 7.9f); break;
        default:
            if (idx >= 0x19 && idx < 0x25 && s.fx)
                s.fx->setParam(idx - 0x19, elementKnobParams(s.raw, patch_.params)[size_t(idx)]);
            break;
    }
}

float PatchPlayer::getParam(int elem, int idx) const {
    if (idx < 0 || idx > 0xff) return 0.f;
    if (elem == 0) return idx < 0xfc ? patch_.params[size_t(idx)] : 0.f;
    if (elem >= 0x41 && elem <= 0x44) return sendFx_[elem - 0x41].raw[size_t(idx)];
    const int rec = elem - 1;
    if (rec < 0 || rec >= int(elemIndex_.size()) || elemIndex_[rec] < 0) return 0.f;
    return elems_[size_t(elemIndex_[rec])].raw[size_t(idx)];
}


}  // namespace hs2
