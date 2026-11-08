// hs2::Engine: the original's synth object (FUN_1007c690: a global part + 16 channel parts) with the main render
// FUN_1007bcf0, and the plugin layer that feeds it (processEvents FUN_1008d5e0, process FUN_1008d400, setParameter
// FUN_1008a950, the patch loader FUN_10089ef0, setChunk FUN_1008c270 for combis). See engine.h and docs/NOTES.md.
#include "hs2/engine.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace hs2 {

namespace {
float bits(uint32_t u) { return std::bit_cast<float>(u); }

// A new part's patch parameters (part ctor FUN_10018b80: part+0x4d0 + 4 idx); the parts the synth ctor creates and
// FUN_1007bbb0(ch, null) keep these (FUN_10016f70 with a null blob loads nothing).
Patch emptyPatch() {
    Patch p;
    p.params = PatchPlayer::defaultParams();
    p.tailMarker = 1;  // not an old patch: no loader fixups
    return p;
}

// FUN_1008a950: part parameter j (2..0x2f) -> patch parameter, -1 = none
int partParamIndex(int j) {
    static constexpr int8_t kMap[0x30] = {
        -1, -1, 0x43, 0x3c, 0x3d, 0x3f, 0x40, 0x41, 0x42, 0x0e, 0x0f, 0x67, 0x69, 0x6b, 0x6d, 0x31,
        0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3a, 0x3e, -1, 0x3b, 0x49, 0x4a, 0x39, 0x03, -1,
        0x4c, 0x4d, 0x4e, 0x50, 0x51, 0x52, 0x53, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, -1, 0x18, 0x19};
    if (j == 0x1f) return 0x94;
    return j >= 0 && j < 0x30 ? kMap[j] : -1;
}

// send FX parameter j (0..0x17 of a 0x18 block) -> element parameter (FUN_1008a950)
int sendFxParamIndex(int j) {
    if (j == 0) return 0;
    if (j <= 0x10) return j + 0x18;
    static constexpr int kTail[7] = {0x14, 0x8b, 0x8c, 0x15, 0x16, 0x17, 0x18};
    return j < 0x18 ? kTail[j - 0x11] : -1;
}
}  // namespace

Engine::Engine(Content& content, float sampleRate, int outputs)
    : content_(content), sr_(sampleRate), outputs_(std::clamp(outputs, 1, 16)) {
    bus_.outputs = outputs_;
    chain_.fill(-1);
    // plugin ctor FUN_1008bad0: VST parameter defaults
    vst_[0] = 0.5f;
    for (int ch = 0; ch < kParts; ++ch) {
        const int b = 3 + ch * kParamsPerPart;
        static constexpr int kOfs[] = {0, 0xd, 0x10, 0x12, 0x13, 0x14, 0x47, 0x48, 0x5f, 0x60, 0x77, 0x78, 0x8f, 0x90};
        for (int o : kOfs) vst_[size_t(b + o)] = (o == 0xd || o == 0x10) ? 1.f : 0.5f;
        vst_[size_t(b + 1)] = bits(0x3d4ccccd);
        vst_[size_t(b + 0x17)] = bits(0x3f356d5d);
    }
    for (int k = 0; k < 4; ++k) {
        vst_[size_t(0x991 + 0x18 * k)] = bits(0x3d4ccccd);
        vst_[size_t(0x992 + 0x18 * k)] = vst_[size_t(0x993 + 0x18 * k)] = 0.5f;
    }
    // resume(): +0xb00 = 1 / (sr * 60); the synth ctor's stored sync: 120 bpm, 4 beats per bar
    hSpb_ = 1.0 / (double(sampleRate) * 60.0);
    hBps_ = 120.0 * hSpb_;
    sBps_ = std::bit_cast<double>(uint64_t(0x3f07c6f8c751f177ull));  // synth ctor: 120 bpm at 44.1 kHz, whatever the rate
    setVelSens(0.5f);
    setTouchSens(0.5f);
    // synth ctor: the global part and one part per channel (no patch), then setSampleRate for all (FUN_1007ba30)
    global_ = newPart();
    global_->load(emptyPatch());
    for (auto& s : slots_) {
        s.cur = newPart();
        s.cur->load(emptyPatch());
    }
    setMaster();  // FUN_1008a950(0, master volume) -> FUN_10089380(hype)
}

Engine::~Engine() = default;

std::unique_ptr<PatchPlayer> Engine::newPart() {
    auto p = std::make_unique<PatchPlayer>(content_, sr_, &rng_);
    p->eng_ = &bus_;
    p->globalPart_ = global_.get();
    p->master_ = master_;
    p->masterTune_ = masterTune_;
    p->sBpm_ = sBpm_;
    p->sBps_ = sBps_;
    p->sBpb_ = sBpb_;
    p->sBar_ = sBar_;
    p->sFlags_ = sFlags_;
    p->setMemoryMode(memory_);
    p->setRingOut(ringOut_);
    return p;
}

void Engine::setRingOut(bool on) {
    ringOut_ = on;
    if (global_) global_->setRingOut(on);
    for (auto& s : slots_) {
        if (s.cur) s.cur->setRingOut(on);
        for (auto& o : s.old) o->setRingOut(on);
        for (auto& o : s.pending) o->setRingOut(on);
        for (auto& f : s.fading) f.p->setRingOut(on);
    }
}

void Engine::resume() { hBpm_ = 0.0; }

// FUN_10089380 (with FUN_1007c610): the tone enhancer (g = hype * 2.2, a = 700 / sr, mode 2 from 64 kHz; off at
// hype <= 0.01) and the master gain content+0x12dc = (8 - g) / 4 * volume^2, read by every part's output gain.
void Engine::setMaster() {
    if (!(hype_ > 0.01f)) {
        enhMode_ = 0;
        enhG_ = 0.f;
    } else {
        enhG_ = float(double(hype_) * double(2.2f));
        enhA_ = float(700.0 / double(sr_));
        enhMode_ = sr_ >= 64000.f ? 2 : 1;
    }
    overflowLogged_ = false;
    const double r = (8.0 - double(enhG_)) * 0.25;
    master_ = float(r * double(vst_[0]) * double(vst_[0]));
    auto upd = [&](PatchPlayer* p) {
        if (!p) return;
        p->master_ = master_;
        p->updateOutputGain();
    };
    upd(global_.get());
    for (auto& s : slots_) {
        upd(s.cur.get());
        for (auto& o : s.old) upd(o.get());
        for (auto& o : s.pending) upd(o.get());
    }
}

void Engine::setHype(float v) {
    hype_ = v;
    setMaster();
}

void Engine::setMemoryMode(float v) {  // FUN_10089340
    memory_ = v;
    for (auto& s : slots_) s.latest()->setParam(0, 0x01, v);
}

void Engine::setMasterTune(int semis, float fine) {  // FUN_10088720
    tuneSemis_ = semis;
    tuneFine_ = fine;
    masterTune_ = float(semis) + fine;
    for (auto& s : slots_) {
        PatchPlayer& p = *s.latest();
        p.masterTune_ = masterTune_;
        p.setParam(0, 0x38, p.getParam(0, 0x38));
    }
}

// FUN_10083ea0: the note-on velocity table (+0xb3f4); 0.5 = linear (verified against the oracle at 0.1 and 0.85).
void Engine::setVelSens(float x) {
    velSens_ = x;
    const double f = double(x + x) - 1.0;
    for (int i = 0; i < 128; ++i) {
        int v;
        if (f >= 0.0) v = int(double(float(i)) * (f * 0.5 + 1.0));
        else v = int(double(i) - ((double(i) * double(i) * 0.007874 + 1.0) - double(i)) * f);
        velTab_[i] = uint8_t(std::min(v, 127));
    }
    velTab_[0] = 0;
}

// FUN_10083fb0: the aftertouch table (+0xb474); 0.5 = linear (verified against the oracle at 0.2 and 0.9).
void Engine::setTouchSens(float x) {
    touchSens_ = x;
    double f = double(x + x) - 1.0, scale = 1.0;
    if (f < 0.0) {
        f += f;
        if (f < -1.0) {
            scale = (f + 2.0) * 0.5;
            f = -1.0;
        }
    }
    for (int i = 0; i < 128; ++i) {
        const double fi = double(i);
        int v;
        if (f < 0.0) {
            v = int((fi - ((fi * fi * fi * 6.8e-05 - 12.0) - fi) * f) * scale);
            v = std::max(v, 0);
        } else {
            v = int((((fi + fi) - fi * fi * 0.006) - fi) * f + fi);
            v = std::min(v, 127);
        }
        touchTab_[i] = uint8_t(v);
    }
    touchTab_[0] = 0;
}

// FUN_1007bbb0 (Engine::loadPatch): a new part (synced to the stored tempo, memory mode in part+0x14), the patch load,
// setter (0x12, 0); the part is the channel's from now on for parameters, and plays from the next render.
void Engine::loadPatchEngine(int ch, const Patch* patch) {
    auto p = newPart();
    p->load(patch ? *patch : emptyPatch());
    p->setParam(0, 0x12, 0.f);
    slots_[size_t(ch)].pending.push_back(std::move(p));
    if (cutOnLoad_) slots_[size_t(ch)].dropOld = true;
}

void Engine::loadPatch(int part, const Patch& patch, int bank) {
    if (part < 0 || part >= kParts) return;
    loadIntoPart(part, &patch, false, bank);
}

// FUN_10089ef0(blob, ch, restore, bank)
void Engine::loadIntoPart(int ch, const Patch* patch, bool restore, int bank) {
    loadPatchEngine(ch, patch);
    PatchPlayer& P = *slots_[size_t(ch)].latest();
    auto get = [&](int idx) { return P.getParam(0, idx); };
    const int P0 = ch * kParamsPerPart;
    auto setP = [&](int j, float v) { setParameter(P0 + j, v); };
    if (int(double(get(0x97)) * 128.9) == 0x80 && bank >= 0 && bank < 100) {  // the default scene root
        P.setParam(0, 0x97, float(double(sceneRoot_) * 0.0078125));
        P.setParam(0, 0x96, float(sceneMidi_));
    }
    if (ch != 0) setParameter(P0, vst_[size_t(P0)]);  // Link again
    P.setParam(0, 0x01, memory_);
    auto settingsFromPatch = [&]() {
        setP(3, get(0x3c));
        for (int k = 0; k < 4; ++k) setP(5 + k, get(0x3f + k));
        for (int k = 0; k < 6; ++k) setP(0xf + k, get(0x31 + k));
        setP(0x15, get(0x37));
        setP(0x16, get(0x38));
        setP(0x17, get(0x3a));
        setP(0x18, get(0x3e));
    };
    if (restore) {
        setP(1, get(0x00));
        setP(2, get(0x43));
        settingsFromPatch();
        setP(4, get(0x3d));
    } else {
        if (!(vst_[size_t(P0 + 0x19)] > 0.5f)) {
            P.setParam(0, 0x00, vst_[size_t(P0 + 1)]);
            if (vst_[size_t(P0 + 2)] < 0.01f) setP(2, get(0x43));
            else P.setParam(0, 0x43, vst_[size_t(P0 + 2)]);
            settingsFromPatch();
        } else {  // "Keep": the part settings stay
            static constexpr int kKeep[][2] = {{1, 0x00}, {2, 0x43}, {3, 0x3c}, {5, 0x3f}, {6, 0x40}, {7, 0x41},
                                               {8, 0x42}, {0xf, 0x31}, {0x10, 0x32}, {0x11, 0x33}, {0x12, 0x34},
                                               {0x13, 0x35}, {0x14, 0x36}, {0x15, 0x37}, {0x16, 0x38}, {0x17, 0x3a},
                                               {0x18, 0x3e}};
            for (const auto& kv : kKeep) P.setParam(0, kv[1], vst_[size_t(P0 + kv[0])]);
        }
        P.setParam(0, 0x3d, vst_[size_t(P0 + 4)]);
    }
    static constexpr int kKnobs[6] = {0x0e, 0x0f, 0x67, 0x69, 0x6b, 0x6d};
    for (int k = 0; k < 6; ++k) setP(9 + k, get(kKnobs[k]));
    static constexpr int kTail[][2] = {{0x1a, 0x3b}, {0x1b, 0x49}, {0x1c, 0x4a}, {0x1d, 0x39}, {0x1e, 0x03},
                                       {0x1f, 0x94}, {0x20, 0x4c}, {0x21, 0x4d}, {0x22, 0x4e}, {0x23, 0x50},
                                       {0x24, 0x51}, {0x25, 0x52}, {0x26, 0x53}, {0x27, 0x11}, {0x28, 0x12},
                                       {0x29, 0x13}, {0x2a, 0x14}, {0x2b, 0x15}, {0x2c, 0x16}};
    for (const auto& kv : kTail) setP(kv[0], get(kv[1]));
    {  // the arp's 0x17 is shown as (v * 1.25 - 0.25), and set back as v' * 0.8 + 0.2
        const double t = double(get(0x17)) * double(1.25f) - double(0.25f);
        setP(0x2d, t >= 0.0 ? float(t) : 0.f);
    }
    setP(0x2e, get(0x18));
    setP(0x2f, get(0x19));
}

// FUN_10089c90: a part that holds a patch (its name is not empty) becomes empty
void Engine::clearPart(int ch) {
    if (ch < 0 || ch >= kParts) return;
    PatchPlayer& old = *slots_[size_t(ch)].latest();
    if (old.patch().name.empty()) return;
    old.setParam(0, 0x3b, 0.f);
    queueSoundOff(ch);
    loadPatchEngine(ch, nullptr);
    const int P0 = ch * kParamsPerPart;
    if (vst_[size_t(P0 + 0x19)] < 0.5f) {
        static constexpr float kDef[0x18] = {0, 0, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0.5f,
                                             0.5f, 0.5f, 0};
        for (int j = 1; j <= 0x18; ++j) setParameter(P0 + j, j == 4 ? bits(0x3d4ccccd) : kDef[j - 1]);
    }
}

// setChunk FUN_1008c270 (a bank chunk = a combi)
void Engine::loadCombi(const Combi& c) {
    for (int k = 0; k < kParts; ++k) {
        if (!c.parts[size_t(k)]) clearPart(k);
        else loadIntoPart(k, &*c.parts[size_t(k)], true, -1);
    }
    for (int i = 0; i < 4; ++i) {
        for (int idx = 0; idx < 0x100; ++idx)
            global_->setParam(0x41 + i, idx, idx == 1 ? 0.f : c.globalFx[size_t(i)].params[size_t(idx)]);
        // FUN_10086cf0: the global FX's VST parameters from the element (each set again through setParameter)
        const int p0 = kGlobalFxBase + i * 0x18;
        for (int j = 0; j < 0x18; ++j) setParameter(p0 + j, global_->getParam(0x41 + i, sendFxParamIndex(j)));
    }
    setParameter(0, c.masterVolume);
    for (int k = 1; k < kParts; ++k) {
        setParameter(k * kParamsPerPart, c.links[size_t(k)] ? 1.f : 0.f);
        setParameter(k * kParamsPerPart + 0x19, 0.f);
    }
    if (c.settingsVersion > 0x33) combiName_ = c.name;
    if (!c.chain.empty() && std::all_of(chain_.begin(), chain_.end(), [](int32_t v) { return v == -1; }))
        setCombiChain(c.chain);  // a stored chain is taken only when the current one is empty
    if (c.hasSetup) {
        setVelSens(c.velSens);
        setTouchSens(c.touchSens);
        setMemoryMode(c.memory);
        if (c.hasTune) {
            setHype(c.hype);
            gm_ = c.gm != 0;
            setMasterTune(c.tuneSemis, c.tuneFine);
        }
    }
}

// FUN_1008a950
void Engine::setParameter(int index, float value) {
    if (index < 0 || index >= kNumParams) return;
    vst_[size_t(index)] = value;
    const int part = index / kParamsPerPart;
    if (part >= kParts) {  // global send FX
        const int k = (index - kGlobalFxBase) / 0x18, j = (index - kGlobalFxBase) % 0x18;
        const int idx = sendFxParamIndex(j);
        if (idx >= 0) global_->setParam(0x41 + k, idx, value);
        return;
    }
    PatchPlayer& P = *slots_[size_t(part)].latest();
    int j = index % kParamsPerPart;
    if (j > 0x37) {  // the part's own send FX s: element 0x41 + s
        const int s = (j - 0x38) / 0x18, jj = (j - 0x38) % 0x18;
        int idx;
        if (jj == 0) idx = 0;
        else if (jj <= 0x10) idx = jj + 0x18;
        else idx = sendFxParamIndex(jj);
        P.setParam(0x41 + s, idx, value);
        return;
    }
    switch (j) {
        case 0:
            if (part == 0) {
                setMaster();
            } else if (value <= 0.5f) {
                if (link_[part - 1]) queueSoundOff(part);
                link_[part - 1] = false;
            } else {
                link_[part - 1] = true;
            }
            return;
        case 1:
            if (value > 0.5f && P.getParam(0, 0x01) != 0.f) queueSoundOff(part);
            P.setParam(0, 0x00, value);
            return;
        case 0x2d: P.setParam(0, 0x17, float(double(value) * double(0.8f) + double(0.2f))); return;
        default: {
            const int idx = partParamIndex(j);
            if (idx >= 0) P.setParam(0, idx, value);
            return;
        }
    }
}

void Engine::setPartSetter(int part, int elem, int idx, float value) {
    if (part < 0) global_->setParam(elem, idx, value);
    else if (part < kParts) slots_[size_t(part)].latest()->setParam(elem, idx, value);
}

float Engine::getPartSetter(int part, int elem, int idx) const {
    if (part < 0) return global_->getParam(elem, idx);
    return part < kParts ? slots_[size_t(part)].latest()->getParam(elem, idx) : 0.f;
}

// FUN_1007baf0: the global part, the stored values for parts loaded later, then each channel's part
void Engine::syncTempo(double bpm, double bps, double bpb, double ppq, double barStart, int flags) {
    global_->syncTempo(bpm, bps, bpb, ppq, barStart, flags);
    sBpm_ = bpm;
    sBps_ = bps;
    sBpb_ = bpb;
    sBar_ = barStart;
    sFlags_ = (flags & 1) | 0x60;
    for (auto& s : slots_) s.latest()->syncTempo(bpm, bps, bpb, ppq, barStart, flags);
}

// FUN_100884c0 (as PatchPlayer::hostSync, for the whole engine)
double Engine::hostSync(const HostTransport* t) {
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
        syncTempo(hBpm_, hBps_, hBpb_, hPos_, hBar_, f6);
    }
    return hPos_;
}

// FUN_100886c0: append to channel ch's list, and to the next channel's while it is linked
void Engine::addEvent(int ch, bool linked, int ofs, uint8_t s, uint8_t d1, uint8_t d2, uint8_t d3) {
    for (int c = ch; c < kParts; ++c) {
        lists_[size_t(c)].push_back(Ev{ofs, {s, d1, d2, d3}});
        if (!(linked && link_[c])) break;
    }
}

// FUN_10087fb0: RPN / NRPN data entry
void Engine::nrpn(int ch, int param, float v) {
    if (param < 0) return;
    const int P0 = ch * kParamsPerPart;
    auto setP = [&](int j, float x) { setParameter(P0 + j, x); };
    switch (param) {
        case 0x00: setP(1, v); break;
        case 0x03: setP(0x1e, v); break;
        case 0x0e: setP(9, v); break;
        case 0x0f: setP(10, v); break;
        case 0x11: setP(0x27, v); break;
        case 0x37: {
            const float x = float((double(v) - 0.5) * 1.78 + 0.5);
            setP(0x15, x < 0.f ? 0.f : std::min(x, 1.f));
            break;
        }
        case 0x38: setP(0x16, v); break;
        case 0x39: setP(0x1d, v); break;
        case 0x3c: setP(3, v); break;
        case 0x3f: case 0x40: case 0x41: case 0x42: setP(5 + param - 0x3f, v); break;
        case 0x43: setP(2, v); break;
        case 0x49: {
            float x = float(double(v) * 8.2);
            if (x > 0.8f) x = 1.f;
            setP(0x1b, x);
            setP(0x1c, x);
            break;
        }
        case 0x4c: setP(0x20, v); break;
        case 0x4d: setP(0x21, v); break;
        case 0x67: setP(0xb, v); break;
        case 0x69: setP(0xc, v); break;
        case 0x6b: setP(0xd, v); break;
        case 0x6d: setP(0xe, v); break;
        case 0x94: setP(0x1f, v); break;
        default:
            if ((param >> 8) < 0x40) slots_[size_t(ch)].latest()->setParam(param >> 8, param & 0xff, v);
            break;
    }
}

// FUN_1008d5e0 (processEvents): per-channel lists with the velocity / touch tables and links; CC0 / CC32 bank select,
// RPN / NRPN (CC 98..101 + data entry CC 6), CC 126 / 127 (part Mono), program changes (requests) stay here.
void Engine::processEvents(const MidiEvent* events, int n, int frames) {
    for (auto& l : lists_) l.clear();
    for (int i = 0; i < n; ++i) {
        const MidiEvent& e = events[i];
        if (e.offset >= frames) continue;  // the original's lists end at the first offset >= the block size
        const int ofs = std::max(0, e.offset);
        const uint8_t st = e.data[0], type = st & 0xf0;
        const int ch = st & 0x0f;
        const uint8_t d1 = e.data[1] & 0x7f, d2 = e.data[2] & 0x7f;
        switch (type) {
            case 0x90:
                if (d2 != 0) {
                    addEvent(ch, true, ofs, 0x90, d1, velTab_[d2], e.data[3]);
                    break;
                }
                [[fallthrough]];
            case 0x80: addEvent(ch, true, ofs, 0x80, d1, 0, 0); break;
            case 0xa0: addEvent(ch, true, ofs, 0xa0, touchTab_[d2], d1, 0); break;
            case 0xd0: addEvent(ch, true, ofs, 0xd0, touchTab_[d1], 0, 0); break;
            case 0xe0: addEvent(ch, true, ofs, 0xe0, d1, d2, 0); break;
            case 0xb0: {
                const float v = d2 == 0x7f ? 1.f : float(int(d2)) * 0.0078125f;
                int& r = rpn_[ch];
                switch (d1) {
                    case 0x00: bankSel_[ch] = int(d2) * 0x80 + (bankSel_[ch] & 0x7f); break;
                    case 0x20: bankSel_[ch] = (bankSel_[ch] & 0x3f80) + int(d2); break;
                    case 0x06:
                        switch (r) {
                            case 0: nrpn(ch, 0x49, v); break;
                            case 1: nrpn(ch, 0x38, v); break;
                            case 2: nrpn(ch, 0x37, v); break;
                            case 3: nrpn(ch, 0x4d, v); break;
                            case 4: nrpn(ch, 0x4c, v); break;
                            case 5: break;
                            default:
                                if (!gm_ && r - 0x4000 != 0x3fff) nrpn(ch, r - 0x4000, v);
                                break;
                        }
                        break;
                    case 0x62: r = (r & 0x3f80) + 0x4000 + int(d2); break;
                    case 0x63: r = (int(d2) + 0x80) * 0x80 + (r & 0x7f); break;
                    case 0x64: r = (r & 0x3f80) + int(d2); break;
                    case 0x65: r = int(d2) * 0x80 + (r & 0x7f); break;
                    case 0x7e:
                    case 0x7f:
                        setParameter(ch * kParamsPerPart + 0x1d, (d1 == 0x7e && d2 != 0) ? 1.f : 0.f);
                        break;
                    case 0x79:
                        r = 0x3fff;
                        addEvent(ch, true, ofs, 0xb0, d1, d2, 0);
                        break;
                    default: addEvent(ch, true, ofs, 0xb0, d1, d2, 0); break;
                }
                break;
            }
            case 0xc0: {  // program change: a load request when it differs from the channel's last one
                const int prog = d1;
                if (combiSel_ == 2 && chain_[size_t(prog)] != -1) {  // Combi Chain: always requested
                    bankProg_[ch] = chain_[size_t(prog)] + 0x10000;
                    requests_.push_back({ch, chain_[size_t(prog)] >> 8, chain_[size_t(prog)] & 0xff});
                    break;
                }
                int req = bankSel_[ch] * 0x100 + prog;
                if (gm_) {
                    const int b = bankSel_[ch] >> 7;
                    bool drums;
                    if (ch == 9) drums = b != 0x79;
                    else drums = b == 0x78 || b == 0xff;
                    req = prog + (drums ? 0x4f00 : 0x4e00);
                    if (b == 0x40 || b == 0x7e) req = 0x7f7f;
                }
                if ((bankProg_[ch] & 0xffff) != req) {
                    bankProg_[ch] = req + 0x10000;
                    requests_.push_back({ch, req >> 8, req & 0xff});
                }
                break;
            }
            default: break;
        }
    }
}

void Engine::setCombiChain(const std::vector<int32_t>& chain) {
    chain_.fill(-1);
    for (size_t i = 0; i < chain.size() && i < chain_.size(); ++i) chain_[i] = chain[i];
}

std::vector<Engine::ProgramRequest> Engine::takeProgramRequests() {
    std::vector<ProgramRequest> r;
    r.swap(requests_);
    return r;
}

void Engine::processBlock(float* outLR, int frames, const MidiEvent* events, int numEvents,
                          const HostTransport* transport) {
    float* outs[16] = {outLR};
    processBlockMulti(outs, frames, events, numEvents, transport);
}

// processEvents + process (FUN_1008d400)
void Engine::processBlockMulti(float* const* outs, int frames, const MidiEvent* events, int numEvents,
                               const HostTransport* transport) {
    if (frames <= 0) return;
    processEvents(events, numEvents, frames);
    // FUN_1008cc20: queued all-sound-off (CC 120) at the channel's last event offset
    for (int ch : ring_) {
        const int ofs = lists_[size_t(ch)].empty() ? 0 : lists_[size_t(ch)].back().offset;
        addEvent(ch, true, ofs, uint8_t(0xb0 + ch), 0x78, 0, 0);
    }
    ring_.clear();
    const double pos = hostSync(transport);
    render(frames, pos);
    for (int p = 0; p < outputs_; ++p) {
        if (!outs[p]) continue;
        const float* src = outBuf_.data() + size_t(p) * 2 * size_t(frames);
        for (int i = 0; i < 2 * frames; ++i) outs[p][i] += src[i];
    }
    firstBlock_ = false;
    hPos_ = double(frames) * hBps_ + hPos_;  // +0xb20 += frames * +0xb10
}

// FUN_1007bcf0: the multi-part render of one block
void Engine::render(int n, double pos) {
    const size_t len = size_t(n) * 2;
    if (outBuf_.size() < len * size_t(outputs_)) outBuf_.resize(len * size_t(outputs_));
    std::fill(outBuf_.begin(), outBuf_.begin() + std::ptrdiff_t(len * size_t(outputs_)), 0.f);
    if (gbusBuf_.size() < len * 4) gbusBuf_.assign(len * 4, 0.f);
    for (int p = 0; p < outputs_; ++p) bus_.out[p] = outBuf_.data() + size_t(p) * len;
    for (int k = 0; k < 4; ++k) bus_.gbus[k] = gbusBuf_.data() + size_t(k) * len;
    if (!firstBlock_)
        for (bool& u : bus_.outUsed) u = false;
    for (int ch = 0; ch < kParts; ++ch) {
        Slot& s = slots_[size_t(ch)];
        if (!s.pending.empty()) {  // the oldest pending part becomes current; the current one sounds on as old
            s.old.insert(s.old.begin(), std::move(s.cur));
            s.cur = std::move(s.pending.front());
            s.pending.erase(s.pending.begin());
        }
        if (s.dropOld && s.pending.empty()) {  // cutOnLoad: the previous patch fades out instead of sounding on
            const int len = std::max(1, int(sr_ * 0.01f));
            for (auto& o : s.old) s.fading.push_back({std::move(o), len, len});
            s.old.clear();
            s.dropOld = false;
        }
        if (!s.fading.empty()) renderFading(s, n, pos);
        bool more = false;  // synth+0x35c is 0
        for (size_t i = 0; i < s.old.size(); ++i) {
            const bool silent = s.old[i]->partRender(nullptr, n, pos, 0);
            if (i + 1 == s.old.size() && (silent || more)) {
                s.old.pop_back();
                break;
            }
            more = true;
        }
        double lp = pos;
        int at = 0;
        for (Ev& e : lists_[size_t(ch)]) {
            if (at < e.offset) {
                s.cur->partRender(nullptr, e.offset - at, lp, at);
                lp = double(e.offset - at) * sBps_ + lp;
                at = e.offset;
            }
            e.m[0] &= 0xf0;
            s.cur->midiEvent(e.m, false);
            for (auto& o : s.old)
                if (e.m[0] != 0x90) o->midiEvent(e.m, false);
        }
        if (at < n) s.cur->partRender(nullptr, n - at, lp, at);
    }
    global_->sendFxRender(n, pos, -1, nullptr);  // FUN_1000f130(.., -1) on the global part
    // overflow guard: |x| >= 100 in the first frames of output channels 1 / 2 clears the outputs and the enhancer
    bool bad = false;
    const float* o0 = bus_.out[0];
    for (int i = 0; i < std::min(n, 4); ++i)
        if (o0[2 * i] <= -100.f || o0[2 * i] >= 100.f || o0[2 * i + 1] <= -100.f || o0[2 * i + 1] >= 100.f) bad = true;
    if (bad) {
        std::memset(enhState_, 0, sizeof enhState_);
        for (int c = 0; c < 2 * outputs_; ++c)
            if (bus_.outUsed[c]) {
                float* o = bus_.out[c / 2];
                for (int i = 0; i < n; ++i) o[2 * i + (c & 1)] = 0.f;
            }
        overflowLogged_ = true;
        return;
    }
    if (enhMode_ == 0) return;
    for (int c = 0; c < 2 * outputs_; ++c)
        if (bus_.outUsed[c]) enhance(bus_.out[c / 2] + (c & 1), n, c);
}

// cutOnLoad (not in the original): each fading part renders into scratch buses, which are added to the real outputs
// and global send buses with a linear fade; a part is dropped when its fade is over. It gets no more MIDI.
void Engine::renderFading(Slot& s, int n, double pos) {
    const size_t len = size_t(n) * 2, total = len * size_t(outputs_ + 4);
    if (fadeBuf_.size() < total) fadeBuf_.resize(total);
    const PartBus saved = bus_;
    PartBus merged = bus_;
    for (auto& f : s.fading) {
        std::fill(fadeBuf_.begin(), fadeBuf_.begin() + std::ptrdiff_t(total), 0.f);
        for (int p = 0; p < outputs_; ++p) {
            bus_.out[p] = fadeBuf_.data() + size_t(p) * len;
            bus_.outUsed[2 * p] = bus_.outUsed[2 * p + 1] = false;
        }
        for (int k = 0; k < 4; ++k) {
            bus_.gbus[k] = fadeBuf_.data() + size_t(outputs_ + k) * len;
            bus_.gused[k] = false;
        }
        f.p->partRender(nullptr, n, pos, 0);
        auto fadeAdd = [&](float* dst, const float* src) {
            for (int i = 0; i < n; ++i) {
                const float g = float(std::max(0, f.left - i)) / float(f.len);
                dst[2 * i] += src[2 * i] * g;
                dst[2 * i + 1] += src[2 * i + 1] * g;
            }
        };
        for (int p = 0; p < outputs_; ++p)
            if (bus_.outUsed[2 * p] || bus_.outUsed[2 * p + 1]) {
                fadeAdd(saved.out[p], bus_.out[p]);
                merged.outUsed[2 * p] = merged.outUsed[2 * p + 1] = true;
            }
        for (int k = 0; k < 4; ++k)
            if (bus_.gused[k]) {
                fadeAdd(saved.gbus[k], bus_.gbus[k]);
                merged.gused[k] = true;
            }
        f.left -= n;
    }
    bus_ = merged;
    s.fading.erase(std::remove_if(s.fading.begin(), s.fading.end(), [](const Slot::Fading& f) { return f.left <= 0; }),
                   s.fading.end());
}

// The tone enhancer of FUN_1007bcf0 on one output channel (stride 2): y = x + g (x + carry), carry = lp2(x) - x,
// lp2 = two one-poles (a). Mode 1 mirrors the x87 code (53-bit registers; the 4-frame unrolled loop keeps some
// values in registers and stores others as floats); state: s1, s2, carry, carry2 (synth+0x158 + 16 ch).
void Engine::enhance(float* x, int n, int c) {
    float* st = enhState_[c];
    const double g = enhG_, a = enhA_;
    if (enhMode_ == 1) {
        double C = st[2], S1 = st[0];
        float s2 = st[1], s1m = 0.f;
        int i = 0;
        bool unrolled = false;
        for (; i + 4 <= n; i += 4) {
            unrolled = true;
            const double x0 = x[2 * i], x1 = x[2 * (i + 1)], x2 = x[2 * (i + 2)], x3 = x[2 * (i + 3)];
            x[2 * i] = float((x0 + C) * g + x0);
            S1 = S1 + (x0 - S1) * a;
            const double S2a = (S1 - double(s2)) * a + double(s2);
            C = S2a - x0;
            x[2 * (i + 1)] = float((x1 + C) * g + x1);
            const double S1b = (x1 - S1) * a + S1;
            s1m = float(S1b);
            const double S2b = (S1b - S2a) * a + S2a;
            C = S2b - x1;
            x[2 * (i + 2)] = float((x2 + C) * g + x2);
            const double S1c = (x2 - double(s1m)) * a + double(s1m);
            s1m = float(S1c);
            const double S2c = (S1c - S2b) * a + S2b;
            s2 = float(S2c);
            C = double(s2) - x2;
            x[2 * (i + 3)] = float((x3 + C) * g + x3);
            S1 = (x3 - double(s1m)) * a + double(s1m);
            const double S2d = (S1 - double(s2)) * a + double(s2);
            s2 = float(S2d);
            C = S2d - x3;
        }
        (void)unrolled;
        for (; i < n; ++i) {
            const double x0 = x[2 * i];
            x[2 * i] = float((x0 + C) * g + x0);
            S1 = S1 + (x0 - S1) * a;
            const double S2 = (S1 - double(s2)) * a + double(s2);
            s2 = float(S2);
            C = S2 - x0;
        }
        st[0] = float(S1);
        st[1] = s2;
        st[2] = float(C);
        st[3] = 0.f;
        return;
    }
    // mode 2 (sample rates from 64 kHz): the output uses the carry of two frames before (every carry is stored as a
    // float); same register / float pattern of the unrolled loop as the disassembly (1007c3eb..1007c5cb)
    double C = st[2], S1 = st[0];
    float s2 = st[1], c2 = st[3], s1m = 0.f;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const double x0 = x[2 * i], x1 = x[2 * (i + 1)], x2 = x[2 * (i + 2)], x3 = x[2 * (i + 3)];
        x[2 * i] = float((x0 + C) * g + x0);
        const float c20 = c2;
        s1m = float((x0 - S1) * a + S1);
        s2 = float((double(s1m) - double(s2)) * a + double(s2));
        const double D0 = double(s2) - x0;
        x[2 * (i + 1)] = float((x1 + double(c20)) * g + x1);
        const double S1b = (x1 - double(s1m)) * a + double(s1m);
        s1m = float(S1b);
        s2 = float((S1b - double(s2)) * a + double(s2));
        const float D0f = float(D0);
        const double E1 = double(s2) - x1;
        x[2 * (i + 2)] = float((double(D0f) + x2) * g + x2);
        const double S1c = (x2 - double(s1m)) * a + double(s1m);
        s1m = float(S1c);
        s2 = float((S1c - double(s2)) * a + double(s2));
        const float E1f = float(E1);
        const float F2f = float(double(s2) - x2);
        x[2 * (i + 3)] = float((x3 + double(E1f)) * g + x3);
        S1 = (x3 - double(s1m)) * a + double(s1m);
        s2 = float((S1 - double(s2)) * a + double(s2));
        C = F2f;
        c2 = float(double(s2) - x3);
    }
    for (; i < n; ++i) {
        const double x0 = x[2 * i];
        x[2 * i] = float((x0 + C) * g + x0);
        S1 = (x0 - S1) * a + S1;
        s2 = float((S1 - double(s2)) * a + double(s2));
        C = c2;
        c2 = float(double(s2) - x0);
    }
    st[0] = float(S1);
    st[1] = s2;
    st[2] = float(C);
    st[3] = c2;
}

}  // namespace hs2
