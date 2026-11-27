#include "PatchEdit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "ParamDefs.h"
#include "PatchLibrary.h"
#include "PluginProcessor.h"  // fromLatin1
#include "hs2/content.h"
#include "hs2/effects.h"
#include "param_tables.h"

namespace hs2port {

namespace tb = hs2m::tables;

namespace {

// ---- the generated tables by parameter index

struct TableIndex {
    std::array<const tb::Param*, 256> patch{}, common{};
    std::array<std::array<const tb::Param*, 256>, 7> type{};
    TableIndex() {
        for (int i = 0; i < tb::kPatchCount; ++i) patch[size_t(tb::kPatch[i].idx)] = &tb::kPatch[i];
        for (int i = 0; i < tb::kElemCommonCount; ++i) common[size_t(tb::kElemCommon[i].idx)] = &tb::kElemCommon[i];
        auto fill = [this](int t, const tb::Param* p, int n) {
            for (int i = 0; i < n; ++i) type[size_t(t)][size_t(p[i].idx)] = &p[i];
        };
        fill(0, tb::kElemType0, tb::kElemType0Count);
        fill(1, tb::kElemType1, tb::kElemType1Count);
        fill(2, tb::kElemType2, tb::kElemType2Count);
        fill(3, tb::kElemType3, tb::kElemType3Count);
        fill(4, tb::kElemType4, tb::kElemType4Count);
        fill(5, tb::kElemType5, tb::kElemType5Count);
        fill(6, tb::kElemType6, tb::kElemType6Count);
    }
    const tb::Param* elem(int t, int idx) const {
        if (idx < 0 || idx > 255) return nullptr;
        if (t >= 0 && t < 7 && type[size_t(t)][size_t(idx)] != nullptr) return type[size_t(t)][size_t(idx)];
        return common[size_t(idx)];
    }
    const tb::Param* patchParam(int idx) const { return idx >= 0 && idx < 256 ? patch[size_t(idx)] : nullptr; }
};
const TableIndex& tables() {
    static const TableIndex t;
    return t;
}

juce::String utf8(const char* s) { return s != nullptr ? juce::String::fromUTF8(s).trim() : juce::String(); }
juce::String centre(const juce::String& s) { return s == "<C>" ? juce::String("centre") : s; }
juce::String percent(float v) { return juce::String(juce::roundToInt(v * 100.f)) + " %"; }
int quant(float v, float k) { return int(v * k); }
float idxValue(int n) { return (float(n) + 0.5f) / 128.9f; }  // the middle of index n's range (trunc(v * 128.9))
int sample1024(float v) { return juce::jlimit(0, 1024, juce::roundToInt(juce::jlimit(0.f, 1.f, v) * 1024.f)); }

juce::String noteName(int n) {  // Hypersonic's convention: note 0 = C-2, 60 = C3
    static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    n = juce::jlimit(0, 127, n);
    return juce::String(kNames[n % 12]) + juce::String(n / 12 - 2);
}

juce::String numText(float x, const char* unit) {
    auto s = juce::String(x, 2);
    if (s.containsChar('.')) {
        while (s.endsWithChar('0')) s = s.dropLastCharacters(1);
        if (s.endsWithChar('.')) s = s.dropLastCharacters(1);
    }
    if (s == "-0") s = "0";
    const auto u = utf8(unit);
    return u.isEmpty() ? s : s + " " + u;
}

juce::String tbText(const tb::Param& t, float v) {
    v = juce::jlimit(0.f, 1.f, v);
    if (t.kind == tb::kEnum) {
        if (t.numSegs == 0) return {};
        int i = 0;
        while (i + 1 < t.numSegs && t.segs[i + 1].from <= v) ++i;
        return centre(utf8(t.segs[i].text));
    }
    const int idx = sample1024(v);
    if (t.kind == tb::kText) return utf8(t.texts[idx]);
    return numText(t.nums[idx], t.unit);
}

std::vector<std::pair<float, juce::String>> tbChoices(const tb::Param& t) {
    std::vector<std::pair<float, juce::String>> out;
    if (t.kind != tb::kEnum) return out;
    for (int i = 0; i < t.numSegs; ++i) {
        const float a = t.segs[i].from, b = i + 1 < t.numSegs ? t.segs[i + 1].from : 1.f;
        if (b <= a) continue;
        out.push_back({(a + b) * 0.5f, centre(utf8(t.segs[i].text))});
    }
    return out;
}

// ---- steps: the value `delta` texts away (enum: segment midpoints; else the next sample whose text differs)

float stepThresholds(const std::vector<float>& from, float v, int delta) {
    if (from.empty()) return v;
    int i = 0;
    while (i + 1 < int(from.size()) && from[size_t(i) + 1] <= v) ++i;
    i = juce::jlimit(0, int(from.size()) - 1, i + delta);
    const float a = from[size_t(i)], b = i + 1 < int(from.size()) ? from[size_t(i) + 1] : 1.f;
    return juce::jlimit(0.f, 1.f, (a + b) * 0.5f);
}

float stepWalk(const std::function<juce::String(float)>& text, float v, int delta, int res = 1024) {
    int s = juce::jlimit(0, res, juce::roundToInt(v * float(res)));
    const int dir = delta > 0 ? 1 : -1;
    for (int n = std::abs(delta); n > 0; --n) {
        const auto t0 = text(float(s) / float(res));
        int k = s;
        while (k + dir >= 0 && k + dir <= res) {
            k += dir;
            if (text(float(k) / float(res)) != t0) break;
        }
        if (k == s) break;
        s = k;
    }
    return float(s) / float(res);
}

float stepTable(const tb::Param& t, float v, int delta) {
    if (t.kind == tb::kEnum) {
        std::vector<float> from;
        for (int i = 0; i < t.numSegs; ++i) from.push_back(t.segs[i].from);
        return stepThresholds(from, v, delta);
    }
    return stepWalk([&t](float x) { return tbText(t, x); }, v, delta);
}

float stepPt(const paramtext::Table& t, float v, int delta) {
    if (t.kind == paramtext::kEnum) {
        std::vector<float> from;
        for (int i = 0; i < t.numSegs; ++i) from.push_back(t.segs[i].from);
        return stepThresholds(from, v, delta);
    }
    return stepWalk([&t](float x) { return tableText(t, x); }, v, delta);
}

float stepFx(const fxtext::Knob& k, float v, int delta) {
    if (k.numSegs == 0) return v;
    int i = 0;
    const int s = sample1024(v);
    while (i + 1 < k.numSegs && k.segs[i + 1].from <= s) ++i;
    i = juce::jlimit(0, k.numSegs - 1, i + delta);
    const int a = k.segs[i].from, b = i + 1 < k.numSegs ? k.segs[i + 1].from - 1 : 1024;
    return float((a + b) / 2) / 1024.f;
}

// ---- the send effect blocks (patch send effects: part j 56.. ; same layout as a global effect slot)

const paramtext::Table* sendTable(int jj) {
    switch (jj) {
        case 0: return &paramtext::kFxMute;
        case 17: return &paramtext::kFxAux;
        case 18: return &paramtext::kFxLevel;
        case 19: return &paramtext::kFxPan;
        case 20: case 21: case 22: case 23: return &paramtext::kFxSend;
        default: return nullptr;
    }
}

const fxtext::Knob* sendKnob(const EditContext& c, int slot, int knob) {
    const int type = c.sendType ? c.sendType(slot) : 128;
    if (type >= 128) return nullptr;
    return fxKnob(type, knob, [&c, slot](int sw) { return c.value(kSendFxElem, kSendFxFirstJ + slot * kSendFxParams + 1 + sw); });
}

const fxtext::Knob* insertKnob(const EditContext& c, int elem, int knob) {
    const int type = quant(c.value(elem, 0x13), 128.9f);
    if (type >= 128) return nullptr;
    return fxKnob(type, knob, [&c, elem](int sw) { return c.value(elem, 0x19 + sw); });
}

int elemTypeOf(const EditContext& c, int elem) { return c.elemType ? c.elemType(elem) : -1; }

// ---- effect types and effect presets (an insert element's Effect Type 0x13, a send effect's kSendTypeIdx + slot)

bool isSendType(int elem, int idx) {
    return elem == kSendFxElem && idx >= kSendTypeIdx && idx < kSendTypeIdx + kSendFxSlots;
}

juce::String typeText(float v) {
    const int t = quant(v, 128.9f);
    return hs2::effectTypeExists(t) ? juce::String(hs2::effectName(t)) : juce::String("No effect");
}

// No effect (type 128), then every effect type the port has, by its number (effects.h).
std::vector<std::pair<float, juce::String>> typeChoices() {
    std::vector<std::pair<float, juce::String>> out{{idxValue(128), "No effect"}};
    for (int t = 0; t < 128; ++t)
        if (hs2::effectTypeExists(t)) out.push_back({idxValue(t), hs2::effectName(t)});
    return out;
}

// A preset row's value: preset k of PatchLibrary::effects, or kCustom (the knobs match no preset).
constexpr float kCustom = 1.f;
float presetValue(int k) { return (float(k) + 0.5f) / 4096.f; }
int presetIndex(const EditContext& c, float v) {
    const int n = c.library != nullptr ? int(c.library->effects.size()) : 0;
    const int k = v >= 0.999f ? -1 : int(v * 4096.f);
    return k >= 0 && k < n ? k : -1;
}

juce::String presetLabel(const EffectPreset& p) {  // "Delay: Mono Delay"
    const juce::String path(p.rel.generic_wstring().c_str());
    const auto folder = path.containsChar('/') ? path.upToFirstOccurrenceOf("/", false, false) : juce::String();
    const auto name = path.fromLastOccurrenceOf("/", false, false).upToLastOccurrenceOf(".", false, false);
    return folder.isNotEmpty() ? folder + ": " + name : name;
}

// Where a preset row's type and knobs are: (type index, first knob index, knob stride 1).
struct PresetSlot { int type, knob; };
PresetSlot presetSlot(int elem, int idx) {
    if (elem == kSendFxElem) {
        const int s = idx - kPresetIdx;
        return {kSendTypeIdx + s, kSendFxFirstJ + s * kSendFxParams + 1};  // send knob j' 1..12
    }
    return {0x13, 0x19};
}

// The preset whose type and twelve knobs the effect has now, or -1.
int currentPreset(const EditContext& c, int elem, int idx) {
    if (c.library == nullptr) return -1;
    const auto at = presetSlot(elem, idx);
    const int type = quant(c.value(elem, at.type), 128.9f);
    std::array<float, 12> knobs{};
    for (int i = 0; i < 12; ++i) knobs[size_t(i)] = c.value(elem, at.knob + i);
    const auto& fx = c.library->effects;
    for (size_t k = 0; k < fx.size(); ++k) {
        if (quant(fx[k].type, 128.9f) != type) continue;
        bool same = true;
        for (int i = 0; same && i < 12; ++i) same = std::abs(fx[k].knobs[size_t(i)] - knobs[size_t(i)]) < 1e-4f;
        if (same) return int(k);
    }
    return -1;
}

std::vector<int> range(int a, int b) {
    std::vector<int> v;
    for (int i = a; i <= b; ++i) v.push_back(i);
    return v;
}

// Keeps the named entries of each group and splits groups longer than kMaxGroupRows.
std::vector<EditGroup> finish(const EditContext& c, int elem, const std::vector<EditGroup>& groups) {
    std::vector<EditGroup> out;
    for (const auto& g : groups) {
        std::vector<int> shown;
        for (int i : g.idx)
            if (editName(c, elem, i).isNotEmpty()) shown.push_back(i);
        if (shown.empty()) continue;
        const int parts = (int(shown.size()) + kMaxGroupRows - 1) / kMaxGroupRows;
        const int per = (int(shown.size()) + parts - 1) / parts;
        for (int k = 0; k < parts; ++k) {
            EditGroup e;
            e.title = parts > 1 ? g.title + ", " + juce::String(k + 1) + " of " + juce::String(parts) : g.title;
            for (int i = k * per; i < std::min(int(shown.size()), (k + 1) * per); ++i) e.idx.push_back(shown[size_t(i)]);
            out.push_back(std::move(e));
        }
    }
    return out;
}

juce::String elemName(const EditContext& c, int elem, int i) {
    const int t = elemTypeOf(c, elem);
    if (t < 0) return {};
    if (t == 0 && i >= 0x19 && i <= 0x24) {
        const auto* k = insertKnob(c, elem, i - 0x19);
        return k != nullptr ? utf8(k->name) : juce::String();
    }
    if (i == 0x13 && t == 0) return "Effect Type";
    if (i == 0x01) return "Element Type";
    switch (i) {
        case 0x04: return "Knob 1 Mode";
        case 0x05: return "Knob 2 Mode";
        case 0x06: return "Knob 1 Dest 1";
        case 0x07: return "Knob 1 Depth 1";
        case 0x08: return "Knob 1 Dest 2";
        case 0x09: return "Knob 1 Depth 2";
        case 0x0a: return "Knob 2 Dest 1";
        case 0x0b: return "Knob 2 Depth 1";
        case 0x0c: return "Knob 2 Dest 2";
        case 0x0d: return "Knob 2 Depth 2";
        case 0x91: return "Knob 1 Source";
        case 0x92: return "Knob 2 Source";
        case 0x55: return "LFO 3 Wave";
        case 0x56: return "LFO 3 Sync";
        case 0x57: return "LFO 3 Rate";
        case 0x58: return "LFO 3 Phase";
        case 0x0e: case 0x0f: case 0x10: return {};
        default: break;
    }
    const tb::Param* tp = tables().elem(t, i);
    if (tp == nullptr || tp->name == nullptr) return {};
    auto n = utf8(tp->name);
    if (n.contains("(test only)")) return {};
    // "Sample / Wave" style names: the sample element's half after the slash, the others' before it
    if (const int sl = n.indexOf(" / "); sl >= 0) n = (t == 2 ? n.substring(sl + 3) : n.substring(0, sl)).trim();
    else if (n.length() > 2 && n.endsWith(" /")) n = t == 2 ? juce::String() : n.dropLastCharacters(2).trim();
    if (n.isEmpty()) return {};
    if (t == 1 && i >= 0x19 && i <= 0x21) n = "Drawbar " + n + "'";
    if (i >= 0xac && i < 0xfc) {  // "Mod 3 Src" -> "Source" (the group names the row)
        static const char* const kField[5] = {"Source", "Modifier", "Mod Parameter", "Destination", "Depth"};
        n = kField[(i - 0xac) % 5];
    }
    return n;
}

juce::String patchName(int i) {
    const tb::Param* tp = tables().patchParam(i);
    if (tp == nullptr || tp->name == nullptr) return {};
    auto n = utf8(tp->name);
    if (n.isEmpty() || n.startsWithChar('<')) return {};
    if (n == "ofs" || n == "rng") {  // the knob range rows: named after the knob entry before them
        for (int b = i - 1; b >= 0 && b >= i - 2; --b)
            if (const tb::Param* kp = tables().patchParam(b); kp != nullptr && kp->name != nullptr &&
                                                                std::strncmp(kp->name, "Knob", 4) == 0)
                return utf8(kp->name) + (n == "ofs" ? " Offset" : " Range");
    }
    if (i >= 0xac && i < 0xfc) {
        static const char* const kField[5] = {"Note", "Velocity", "Gate", "Seq 1", "Seq 2"};
        return kField[(i - 0xac) % 5];
    }
    switch (i) {
        case 0x55: return "LFO 1 Wave";
        case 0x56: return "LFO 1 Sync";
        case 0x57: return "LFO 1 Rate";
        case 0x58: return "LFO 1 Phase";
        case 0x59: return "LFO 2 Wave";
        case 0x5a: return "LFO 2 Sync";
        case 0x5b: return "LFO 2 Rate";
        case 0x5c: return "LFO 2 Phase";
        case 0x1a: return "Phrase Bank";
        case 0x1b: return "Phrase Program";
        case 0x1c: return "Phrase Transpose";
        case 0x21: return "Phrase Scale";
        default: return n;
    }
}

juce::String sendName(const EditContext& c, int j) {
    const int s = (j - kSendFxFirstJ) / kSendFxParams, jj = (j - kSendFxFirstJ) % kSendFxParams;
    if (jj >= 1 && jj <= 16) {
        const auto* k = sendKnob(c, s, jj - 1);
        return k != nullptr ? utf8(k->name) : juce::String();
    }
    static const char* const kTail[7] = {"Aux Output", "Level", "Pan", "Send to effect 1", "Send to effect 2",
                                         "Send to effect 3", "Send to effect 4"};
    if (jj == 0) return "Mute";
    return jj >= 17 && jj < 24 ? juce::String(kTail[jj - 17]) : juce::String();
}

bool isSend(int elem, int idx) {
    return elem == kSendFxElem && idx >= kSendFxFirstJ && idx < kSendFxFirstJ + kSendFxSlots * kSendFxParams;
}

juce::String bankName(const EditContext& c, int type, int n) {
    if (c.library != nullptr && n >= 0 && n <= 128) {
        const auto& names = type == 2 ? c.library->sampleBankNames : c.library->wavetableNames;
        if (!names[size_t(n)].empty()) return fromLatin1(names[size_t(n)]);
    }
    return "bank " + juce::String(n) + " (none)";
}

// A wave of wavetable bank `bank`: "12 Organ 3", or "wave 12" when the bank's names are not known.
const std::vector<std::string>* waveNames(const EditContext& c, int bank) {
    if (c.library == nullptr || bank < 0 || bank > 128) return nullptr;
    const auto& w = c.library->wavetableWaves[size_t(bank)];
    return w.empty() ? nullptr : &w;
}

juce::String waveText(const EditContext& c, int bank, int w, const char* fallback) {
    if (const auto* names = waveNames(c, bank); names != nullptr && w >= 0 && w < int(names->size())) {
        const auto n = fromLatin1((*names)[size_t(w)]);
        return juce::String(w + 1) + " " + (n.isEmpty() ? juce::String("(unnamed)") : n);
    }
    return juce::String(fallback) + " " + juce::String(w + 1);
}

// The waves of the bank by name; empty (a slider) when the names are not known.
std::vector<std::pair<float, juce::String>> waveChoices(const EditContext& c, int bank, const char* fallback) {
    std::vector<std::pair<float, juce::String>> out;
    if (const auto* names = waveNames(c, bank))
        for (int w = 0; w < std::min(int(names->size()), 129); ++w)
            out.push_back({idxValue(w), waveText(c, bank, w, fallback)});
    return out;
}

int sampleCount(const EditContext& c, int elem) {
    if (c.content == nullptr) return 0;
    hs2::SampleBank* sb = c.content->sampleBank(quant(c.value(elem, 0x12), 128.9f));
    return sb != nullptr ? int(sb->instruments.size()) : 0;
}

juce::String sampleText(const EditContext& c, int elem, int w) {
    hs2::SampleBank* sb = c.content != nullptr ? c.content->sampleBank(quant(c.value(elem, 0x12), 128.9f)) : nullptr;
    if (sb != nullptr && w >= 0 && w < int(sb->instruments.size())) {
        const auto n = fromLatin1(sb->instruments[size_t(w)].name);
        return juce::String(w + 1) + " " + (n.isEmpty() ? juce::String("(unnamed)") : n);
    }
    return juce::String(w + 1) + " (none)";
}

}  // namespace

juce::String elementTypeName(int t) {
    static const char* const kNames[] = {"Effect", "Drawbar", "Sample", "Wavetable", "FM", "Analog", "Model"};
    return t >= 0 && t < 7 ? juce::String(kNames[t]) : juce::String("Element");
}

// ---- groups (the Schwung module's editor pages)

std::vector<EditGroup> elementGroups(const EditContext& c, int elem) {
    const int t = elemTypeOf(c, elem);
    if (t < 0) return {};
    std::vector<EditGroup> g;
    const std::vector<int> quick = {0x91, 0x04, 0x06, 0x07, 0x08, 0x09, 0x92, 0x05, 0x0a, 0x0b, 0x0c, 0x0d};
    if (t == 0) {
        std::vector<int> fx = {0x00, 0x13, kPresetIdx};
        for (int i = 0x19; i <= 0x24; ++i) fx.push_back(i);
        for (int i = 0x14; i <= 0x18; ++i) fx.push_back(i);
        g.push_back({"Effect", fx});
        g.push_back({"Quick Knobs", quick});
    } else {
        g.push_back({"Main", {0x00, 0x01, 0x02, 0x03, 0x8a, 0x93, 0x95, 0x90, 0x94}});
        g.push_back({elementTypeName(t), range(0x11, 0x30)});
        g.push_back({"Key and Velocity", range(0x31, 0x38)});
        g.push_back({"Voice", range(0x39, 0x45)});
        g.push_back({"Pitch", range(0x46, 0x4b)});
        g.push_back({"Glide", range(0x4c, 0x54)});
        g.push_back({"LFO 3", range(0x55, 0x58)});
        g.push_back({"Envelope 1", range(0x59, 0x61)});
        g.push_back({"Waveform", range(0x62, 0x66)});
        g.push_back({"Filter", range(0x67, 0x6d)});
        g.push_back({"Filter Envelope", range(0x6e, 0x78)});
        auto amp = range(0x7e, 0x89);
        amp.insert(amp.end(), {0x7b, 0x7c, 0x7d});
        g.push_back({"Amp Envelope", amp});
        g.push_back({"Output", range(0x8b, 0x8f)});
        g.push_back({"Quick Knobs", quick});
        for (int r = 0; r < 16; ++r) {  // "Mod 3: LFO 1 > Pitch"
            const int b = 0xac + 5 * r;
            const auto src = editText(c, elem, b, c.value(elem, b)), dst = editText(c, elem, b + 3, c.value(elem, b + 3));
            const bool off = src.isEmpty() || src.equalsIgnoreCase("off") || src == "-";
            g.push_back({"Mod " + juce::String(r + 1) + ": " + (off ? juce::String("off") : src + " to " + dst), range(b, b + 4)});
        }
    }
    return finish(c, elem, g);
}

std::vector<EditGroup> patchGroups(const EditContext& c) {
    std::vector<EditGroup> g;
    g.push_back({"Patch", {0x01, 0x02, 0x44, 0x45, 0x46}});
    g.push_back({"Piano", {0x54, 0x48, 0x47, 0x4b}});
    g.push_back({"Patch LFOs", range(0x55, 0x5c)});
    auto knobs = range(0x70, 0x78);
    for (int i : {0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x7b, 0x7c, 0x7d}) knobs.push_back(i);
    g.push_back({"Knob Ranges", knobs});
    g.push_back({"GM Controls", range(0x7e, 0x87)});
    g.push_back({"Scenes", {0x95, 0x96, 0x97}});
    g.push_back({"Arp Phrase", {0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x21}});
    for (int s = 0; s < 16; ++s) g.push_back({"Arp Step " + juce::String(s + 1), range(0xac + 5 * s, 0xac + 5 * s + 4)});
    return finish(c, 0, g);
}

EditGroup sendFxGroup(const EditContext& c, int slot) {
    const int type = c.sendType ? c.sendType(slot) : 128;
    const char* fx = type < 128 ? hs2::effectName(type) : nullptr;
    EditGroup g;
    g.title = "Send effect " + juce::String(slot + 1) + ": " + (fx != nullptr ? juce::String(fx) : juce::String("none"));
    const int base = kSendFxFirstJ + slot * kSendFxParams;
    // The effect type and preset, then Mute, Level, Pan, Aux Output, the effect's knobs, then the sends (as the global
    // effects page)
    g.idx = {kSendTypeIdx + slot, kPresetIdx + slot};
    std::vector<int> order = {0, 18, 19, 17};
    for (int k = 1; k <= 16; ++k) order.push_back(k);
    for (int k = 20; k <= 23; ++k) order.push_back(k);
    for (int jj : order)
        if (editName(c, kSendFxElem, base + jj).isNotEmpty()) g.idx.push_back(base + jj);
    return g;
}

// ---- names, texts, choices, steps

juce::String editName(const EditContext& c, int elem, int idx) {
    if (editIsPreset(elem, idx)) return elem == kSendFxElem || elemTypeOf(c, elem) == 0 ? "Effect Preset" : juce::String();
    if (isSendType(elem, idx)) return "Effect Type";
    if (elem == kSendFxElem) return isSend(elem, idx) ? sendName(c, idx) : juce::String();
    if (elem == 0) return patchName(idx);
    return elemName(c, elem, idx);
}

bool editReadOnly(const EditContext& c, int elem, int idx) {
    juce::ignoreUnused(c);
    return elem > 0 && idx == 0x01;
}

bool editIsPreset(int elem, int idx) {
    if (elem == kSendFxElem) return idx >= kPresetIdx && idx < kPresetIdx + kSendFxSlots;
    return elem > 0 && idx == kPresetIdx;
}

float editValue(const EditContext& c, int elem, int idx) {
    if (!editIsPreset(elem, idx)) return c.value(elem, idx);
    const int k = currentPreset(c, elem, idx);
    return k >= 0 ? presetValue(k) : kCustom;
}

std::vector<std::pair<int, float>> presetEdits(const EditContext& c, int elem, int idx, float v) {
    std::vector<std::pair<int, float>> out;
    const int k = editIsPreset(elem, idx) ? presetIndex(c, v) : -1;
    if (k < 0) return out;
    const auto& p = c.library->effects[size_t(k)];
    const auto at = presetSlot(elem, idx);
    out.push_back({at.type, p.type});
    for (int i = 0; i < 12; ++i) out.push_back({at.knob + i, p.knobs[size_t(i)]});
    return out;
}

std::vector<int> presetTargets(int elem, int idx) {
    std::vector<int> out;
    if (!editIsPreset(elem, idx)) return out;
    const auto at = presetSlot(elem, idx);
    out.push_back(at.type);
    for (int i = 0; i < 12; ++i) out.push_back(at.knob + i);
    return out;
}

juce::String editText(const EditContext& c, int elem, int idx, float v) {
    if (editIsPreset(elem, idx)) {
        const int k = presetIndex(c, v);
        return k >= 0 ? presetLabel(c.library->effects[size_t(k)]) : juce::String("Custom (no preset)");
    }
    if (isSendType(elem, idx)) return typeText(v);
    if (elem == kSendFxElem) {
        if (!isSend(elem, idx)) return {};
        const int s = (idx - kSendFxFirstJ) / kSendFxParams, jj = (idx - kSendFxFirstJ) % kSendFxParams;
        if (jj >= 1 && jj <= 16) {
            const auto* k = sendKnob(c, s, jj - 1);
            return k != nullptr ? fxText(*k, v) : juce::String((c.sendType ? c.sendType(s) : 128) >= 128 ? "no effect" : "unused");
        }
        const auto* t = sendTable(jj);
        return t != nullptr ? tableText(*t, v) : percent(v);
    }
    if (elem == 0) {
        if (idx >= 0xac && idx < 0xfc && (idx - 0xac) % 5 == 0) return noteName(quant(v, 128.9f));  // arp step note
        const tb::Param* tp = tables().patchParam(idx);
        return tp != nullptr ? tbText(*tp, v) : percent(v);
    }
    const int t = elemTypeOf(c, elem);
    if (t == 5 && idx == 0x22) return waveText(c, 0, quant(v, 128.9f), "wavetable");  // Analog: bank 0's waves
    if (t == 0 && idx >= 0x19 && idx <= 0x24) {
        const auto* k = insertKnob(c, elem, idx - 0x19);
        return k != nullptr ? fxText(*k, v) : percent(v);
    }
    if (t == 0 && idx == 0x13) return typeText(v);
    if (idx == 0x01) return elementTypeName(quant(v, 15.9f));
    if ((t == 2 || t == 3) && idx == 0x12) return bankName(c, t, quant(v, 128.9f));
    if (t == 2 && idx == 0x13) return sampleText(c, elem, quant(v, 128.9f));
    if (t == 3 && idx == 0x13) return waveText(c, quant(c.value(elem, 0x12), 128.9f), quant(v, 128.9f), "wave");
    const tb::Param* tp = tables().elem(t, idx);
    return tp != nullptr ? tbText(*tp, v) : percent(v);
}

std::vector<std::pair<float, juce::String>> editChoices(const EditContext& c, int elem, int idx) {
    std::vector<std::pair<float, juce::String>> out;
    if (editReadOnly(c, elem, idx)) {  // one entry: the value
        const float v = c.value(elem, idx);
        out.push_back({v, editText(c, elem, idx, v)});
        return out;
    }
    if (editIsPreset(elem, idx)) {  // Custom, then the content's effect presets by folder
        out.push_back({kCustom, "Custom (no preset)"});
        if (c.library != nullptr)
            for (size_t k = 0; k < c.library->effects.size(); ++k)
                out.push_back({presetValue(int(k)), presetLabel(c.library->effects[k])});
        return out;
    }
    if (isSendType(elem, idx) || (elem > 0 && idx == 0x13 && elemTypeOf(c, elem) == 0)) return typeChoices();
    if (elem == kSendFxElem) {
        if (!isSend(elem, idx)) return out;
        const int s = (idx - kSendFxFirstJ) / kSendFxParams, jj = (idx - kSendFxFirstJ) % kSendFxParams;
        if (jj >= 1 && jj <= 16) {
            if (const auto* k = sendKnob(c, s, jj - 1)) out = fxChoices(*k);
        } else if (const auto* t = sendTable(jj)) {
            out = tableChoices(*t);
        }
        return out;
    }
    if (elem == 0) {
        if (idx >= 0xac && idx < 0xfc && (idx - 0xac) % 5 == 0) return out;  // a note: slider
        if (const tb::Param* tp = tables().patchParam(idx); tp != nullptr && tp->numSegs <= 64) out = tbChoices(*tp);
        return out;
    }
    const int t = elemTypeOf(c, elem);
    if (t < 0) return out;
    if (t == 0 && idx >= 0x19 && idx <= 0x24) {
        if (const auto* k = insertKnob(c, elem, idx - 0x19)) out = fxChoices(*k);
        return out;
    }
    if ((t == 2 || t == 3) && idx == 0x12) {  // the banks there are, and the current one if it is missing
        const int cur = quant(c.value(elem, idx), 128.9f);
        for (int n = 0; n <= 128; ++n) {
            const bool exists = c.library != nullptr &&
                                !(t == 2 ? c.library->sampleBankNames : c.library->wavetableNames)[size_t(n)].empty();
            if (exists || n == cur) out.push_back({idxValue(n), bankName(c, t, n)});
        }
        return out;
    }
    if (t == 2 && idx == 0x13) {  // the bank's instruments
        const int count = std::min(sampleCount(c, elem), 129);
        for (int w = 0; w < count; ++w) out.push_back({idxValue(w), sampleText(c, elem, w)});
        return out;
    }
    // the waves by name (a slider when the bank's names are not known)
    if (t == 3 && idx == 0x13) return waveChoices(c, quant(c.value(elem, 0x12), 128.9f), "wave");
    if (t == 5 && idx == 0x22) return waveChoices(c, 0, "wavetable");
    if (const tb::Param* tp = tables().elem(t, idx); tp != nullptr && tp->numSegs <= 64) out = tbChoices(*tp);
    return out;
}

float editStep(const EditContext& c, int elem, int idx, float v, int delta) {
    if (delta == 0 || editReadOnly(c, elem, idx) || editIsPreset(elem, idx)) return v;
    auto res = [&](int r) { return juce::jlimit(0.f, 1.f, float(juce::roundToInt(v * float(r)) + delta) / float(r)); };
    // a list of named values (effect types, wavetables): the next entry
    auto byChoice = [&] {
        const auto choices = editChoices(c, elem, idx);
        if (choices.empty()) return v;
        const auto text = editText(c, elem, idx, v);
        int pos = 0;
        for (size_t q = 0; q < choices.size(); ++q)
            if (choices[q].second == text) pos = int(q);
        return choices[size_t(juce::jlimit(0, int(choices.size()) - 1, pos + delta))].first;
    };
    if (isSendType(elem, idx) || (elem > 0 && idx == 0x13 && elemTypeOf(c, elem) == 0)) return byChoice();
    if (elem == kSendFxElem) {
        if (!isSend(elem, idx)) return v;
        const int s = (idx - kSendFxFirstJ) / kSendFxParams, jj = (idx - kSendFxFirstJ) % kSendFxParams;
        if (jj >= 1 && jj <= 16) {
            const auto* k = sendKnob(c, s, jj - 1);
            return k != nullptr ? stepFx(*k, v, delta) : res(100);
        }
        const auto* t = sendTable(jj);
        return t != nullptr ? stepPt(*t, v, delta) : res(100);
    }
    if (elem == 0) {
        if (idx >= 0xac && idx < 0xfc && (idx - 0xac) % 5 == 0)
            return idxValue(juce::jlimit(0, 127, quant(v, 128.9f) + delta));
        const tb::Param* tp = tables().patchParam(idx);
        return tp != nullptr ? stepTable(*tp, v, delta) : res(100);
    }
    const int t = elemTypeOf(c, elem);
    if (t < 0) return v;
    if ((t == 5 && idx == 0x22) || (t == 3 && idx == 0x13)) {  // the named waves, else the number
        if (!editChoices(c, elem, idx).empty()) return byChoice();
        return idxValue(juce::jlimit(0, 127, quant(v, 128.9f) + delta));
    }
    if (t == 0 && idx >= 0x19 && idx <= 0x24) {
        const auto* k = insertKnob(c, elem, idx - 0x19);
        return k != nullptr ? stepFx(*k, v, delta) : res(100);
    }
    if ((t == 2 || t == 3) && idx == 0x12) {  // the banks there are
        const auto choices = editChoices(c, elem, idx);
        const int cur = quant(v, 128.9f);
        int pos = 0;
        for (size_t q = 0; q < choices.size(); ++q)
            if (quant(choices[q].first, 128.9f) == cur) pos = int(q);
        pos = juce::jlimit(0, juce::jmax(0, int(choices.size()) - 1), pos + delta);
        return choices.empty() ? v : choices[size_t(pos)].first;
    }
    if (idx == 0x13 && (t == 2 || t == 3)) {
        const int count = t == 2 ? sampleCount(c, elem) : 128;
        if (count == 0) return v;
        return idxValue(juce::jlimit(0, std::min(count, 129) - 1, quant(v, 128.9f) + delta));
    }
    const tb::Param* tp = tables().elem(t, idx);
    return tp != nullptr ? stepTable(*tp, v, delta) : res(100);
}

// ---- Keep Settings and Level Trim (part parameters 25 / 26; Level Trim reads as patch parameter 0x3b)

juce::String keepText(float v) { return v > 0.5f ? "keep" : "from the patch"; }

juce::String levelTrimText(float v) {
    const tb::Param* tp = tables().patchParam(0x3b);
    return tp != nullptr ? tbText(*tp, v) : percent(v);
}

std::vector<std::pair<float, juce::String>> levelTrimChoices() {
    const tb::Param* tp = tables().patchParam(0x3b);
    return tp != nullptr && tp->numSegs <= 64 ? tbChoices(*tp) : std::vector<std::pair<float, juce::String>>{};
}

}  // namespace hs2port
