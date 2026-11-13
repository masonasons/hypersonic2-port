// A stand-in VST 2 synth for testing hs2capture without Windows: like the original, it takes CC0 / CC32 bank select
// and program changes, but "loads" the patch only from idle calls (effIdle / effEditIdle), a few calls later.
// Bank 99 is refused (never loads). Patch index 5 is silent. Its state chunk names the loaded patch.
#include "../vst2abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Voice {
    int note = -1;
    double phase = 0, amp = 0, gain = 0;
    bool held = false;
};

struct Mock {
    AEffect fx{};
    AudioMasterCallback host = nullptr;
    double sr = 44100;
    int bankSel[16] = {};
    int pendingBank = -1, pendingProg = -1, idleCount = 0;
    int bank = -1, prog = -1;
    Voice voices[16];
    std::string chunk;
    bool timeOk = true;
};

Mock* self(AEffect* e) { return static_cast<Mock*>(e->object); }

intptr_t VSTCALL dispatch(AEffect* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt) {
    Mock* m = self(e);
    (void)value;
    switch (op) {
        case effClose: delete m; return 0;
        case effSetSampleRate: m->sr = opt; return 0;
        case effGetEffectName: std::strcpy(static_cast<char*>(ptr), "Mock Hypersonic"); return 1;
        case effGetVendorString: std::strcpy(static_cast<char*>(ptr), "hs2capture test"); return 1;
        case effIdle:
        case effEditIdle:
            if (m->pendingProg >= 0 && ++m->idleCount >= 10) {
                if (m->pendingBank != 99) { m->bank = m->pendingBank; m->prog = m->pendingProg; }
                m->pendingBank = m->pendingProg = -1;
            }
            return 1;
        case effGetChunk: {
            char head[64];
            std::snprintf(head, sizeof(head), "%s %d:%d time=%d", index ? "MOCKPROG" : "MOCKBANK", m->bank, m->prog,
                          m->timeOk ? 1 : 0);
            m->chunk = std::string(head) + std::string(index ? 100 : 1000, 'x');
            *static_cast<void**>(ptr) = m->chunk.data();
            return intptr_t(m->chunk.size());
        }
        case effProcessEvents: {
            const auto* ev = static_cast<const VstEventsN*>(ptr);
            for (int i = 0; i < ev->numEvents; ++i) {
                const auto* me = ev->events[i];
                if (me->type != kVstMidiType) continue;
                const int st = uint8_t(me->midiData[0]) & 0xf0, ch = uint8_t(me->midiData[0]) & 0x0f;
                const int d1 = uint8_t(me->midiData[1]), d2 = uint8_t(me->midiData[2]);
                if (st == 0xb0 && d1 == 0) m->bankSel[ch] = d2 << 7 | (m->bankSel[ch] & 0x7f);
                else if (st == 0xb0 && d1 == 0x20) m->bankSel[ch] = (m->bankSel[ch] & 0x3f80) | d2;
                else if (st == 0xb0 && (d1 == 0x78 || d1 == 0x7b)) {
                    for (auto& v : m->voices) v.held = false;
                } else if (st == 0xc0) {
                    m->pendingBank = m->bankSel[ch];
                    m->pendingProg = d1;
                    m->idleCount = 0;
                } else if (st == 0x90 && d2 > 0) {
                    for (auto& v : m->voices)
                        if (v.amp < 1e-7) { v = Voice{d1, 0, 1, d2 / 127.0 * 0.5, true}; break; }
                } else if (st == 0x80 || (st == 0x90 && d2 == 0)) {
                    for (auto& v : m->voices) if (v.note == d1) v.held = false;
                }
            }
            return 1;
        }
        default: return 0;
    }
}

void VSTCALL process(AEffect* e, float** in, float** out, int32_t n) {
    (void)in;
    Mock* m = self(e);
    const auto* t = reinterpret_cast<const VstTimeInfo*>(m->host(e, audioMasterGetTime, 0, 0, nullptr, 0));
    if (!t || std::fabs(t->tempo - 120) > 1e-9) m->timeOk = false;
    for (int c = 0; c < 4; ++c) std::memset(out[c], 0, sizeof(float) * size_t(n));
    if (m->prog < 0 || m->prog == 5) return;  // nothing loaded, or the silent patch
    const double rel = std::exp(-1.0 / (0.1 * m->sr));
    for (auto& v : m->voices) {
        if (v.amp < 1e-7) continue;
        const double f = 440.0 * std::pow(2.0, (v.note - 69) / 12.0);
        for (int i = 0; i < n; ++i) {
            const float s = float(std::sin(v.phase) * v.amp * v.gain);
            out[0][i] += s;
            out[1][i] += s;
            v.phase += 2 * M_PI * f / m->sr;
            if (!v.held) v.amp *= rel;
        }
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(AudioMasterCallback host) {
    if (host(nullptr, audioMasterVersion, 0, 0, nullptr, 0) < 2400) return nullptr;
    Mock* m = new Mock;
    m->host = host;
    AEffect& fx = m->fx;
    fx.magic = kVstMagic;
    fx.dispatcher = dispatch;
    fx.processReplacing = process;
    fx.numOutputs = 4;
    fx.numPrograms = 1;
    fx.flags = effFlagsCanReplacing | effFlagsProgramChunks | effFlagsIsSynth;
    fx.uniqueID = ('S' << 24) | ('t' << 16) | ('S' << 8) | 'S';
    fx.version = 2100;
    fx.object = m;
    return &fx;
}
