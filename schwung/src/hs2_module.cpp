// Hypersonic 2 for Schwung (Ableton Move): the hs2 engine as a Schwung v2 sound generator.
//
// Four parts (the engine's parts 1..4), each listening on any MIDI channel (default part N = channel N). One instance
// receives every channel (module.json: default_forward_channel -2 = passthrough) and routes each message to the parts
// that listen on its channel. The engine runs at Move's 44.1 kHz in Move's 128-frame blocks; its tempo-synced effects
// and arpeggiators follow Move's clock (host get_bpm / get_beat_position / get_clock_status as the VST transport).
//
// Loads run on a worker thread (bank scan, patch parse, sample-bank warm-up), then go to the audio thread as
// commands, as the JUCE plugin does. Everything the engine touches is guarded by Instance::mu (held briefly by
// get_param / set_param, and by render_block for one block).
//
// The UI (ui.js) talks to this file through string keys; see README.md "DSP keys". Every editable value has an
// address (see parseAddr) with three keys: n:<addr> its name, t:<addr> its value text (the original's own texts),
// step:<addr> = delta (the next/previous value the text shows), so ui.js stays generic and every change is spoken.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "FxText.h"
#include "ParamText.h"
#include "PatchLibrary.h"
#include "hs2/bank.h"
#include "hs2/effects.h"
#include "hs2/engine.h"
#include "hs2/patch.h"
#include "hs2/synth.h"
#include "param_tables.h"
#include "plugin_api_v1.h"

namespace fs = std::filesystem;
using hs2port::fxtext::Knob;
namespace pt = hs2port::paramtext;
namespace tb = hs2m::tables;

namespace {

const host_api_v1_t* g_host = nullptr;

void logf(const char* fmt, ...) {
    if (g_host == nullptr || g_host->log == nullptr) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[540];
    std::snprintf(line, sizeof(line), "hs2: %s", buf);
    g_host->log(line);
}

constexpr int kParts = 4;           // Move has four tracks
constexpr int kAllChannels = 16;    // a part's channel setting: 0..15 = channel 1..16, 16 = all channels
constexpr int kKnobs = 6;           // Hyperknobs
constexpr int kPP = hs2::Engine::kParamsPerPart;

// ------------------------------------------------------------------------------------------------ text helpers

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Patch and element names are Latin-1. The Move display font and the JSON to the UI want plain ASCII.
std::string ascii(const std::string& latin1) {
    static const char* const kHi[96] = {
        " ", "!", "c", "L", "o", "Y", "|", "S", "\"", "(c)", "a", "<<", "-", "-", "(r)", "-",
        "o", "+-", "2", "3", "'", "u", "P", ".", ",", "1", "o", ">>", "1/4", "1/2", "3/4", "?",
        "A", "A", "A", "A", "Ae", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "Oe", "x", "O", "U", "U", "U", "Ue", "Y", "Th", "ss",
        "a", "a", "a", "a", "ae", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "oe", "/", "o", "u", "u", "u", "ue", "y", "th", "y"};
    std::string out;
    for (unsigned char c : latin1) {
        if (c == 0) break;
        if (c >= 32 && c < 127) out += char(c);
        else if (c >= 160) out += kHi[c - 160];
        else out += ' ';
    }
    return trim(out);
}

// UTF-8 texts of the effect tables (degree signs, ...): ASCII for the display.
std::string asciiUtf8(const char* s) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        if (*p < 128) out += char(*p);
        else if (*p == 0xc2 && p[1] == 0xb0) out += " deg", ++p;
        else if (*p == 0xc2 && p[1] == 0xb5) out += "u", ++p;
        else if ((*p & 0xc0) == 0x80) continue;
        else out += '?';
    }
    return trim(out);
}

std::string jstr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 32) out += ' ';
        else out += char(c);
    }
    return out + "\"";
}

std::string fnum(double x) {  // round-trips a float
    char b[32];
    std::snprintf(b, sizeof(b), "%.9g", x);
    return b;
}

std::string percent(float v) { return std::to_string(int(std::lround(double(v) * 100.0))) + " %"; }

std::string gainText(double g) {
    if (g <= 0.0) return "off";
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f dB", 20.0 * std::log10(g));
    return b;
}

// tableText (plugin/Source/ParamDefs.h) without JUCE: the original's text of a generated table at value v.
std::string numText(float x, const char* unit) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", double(x));
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    const std::string u = unit ? unit : "";
    return u.empty() ? s : s + " " + u;
}

std::string centre(std::string s) { return s == "<C>" ? std::string("centre") : s; }

int sample1024(float v) { return std::clamp(int(std::lround(double(std::clamp(v, 0.f, 1.f)) * 1024.0)), 0, 1024); }

std::string ptText(const pt::Table& t, float v) {
    v = std::clamp(v, 0.f, 1.f);
    if (t.kind == pt::kEnum) {
        if (t.numSegs == 0) return {};
        int i = 0;
        while (i + 1 < t.numSegs && t.segs[i + 1].from <= v) ++i;
        return centre(trim(t.segs[i].text));
    }
    const int idx = sample1024(v);
    if (t.kind == pt::kText) return trim(t.texts[idx]);
    return numText(t.nums[idx], t.unit);
}

std::string tbText(const tb::Param& t, float v) {
    v = std::clamp(v, 0.f, 1.f);
    if (t.kind == tb::kEnum) {
        if (t.numSegs == 0) return {};
        int i = 0;
        while (i + 1 < t.numSegs && t.segs[i + 1].from <= v) ++i;
        return centre(trim(t.segs[i].text));
    }
    const int idx = sample1024(v);
    if (t.kind == tb::kText) return trim(t.texts[idx]);
    return numText(t.nums[idx], t.unit);
}

std::string fxText(const Knob& k, float v) {
    const int s = sample1024(v);
    int i = 0;
    while (i + 1 < k.numSegs && k.segs[i + 1].from <= s) ++i;
    return asciiUtf8(k.segs[i].text);
}

template <typename SwitchValue>
const Knob* fxKnob(int type, int knob, SwitchValue switchValue) {
    const auto* e = hs2port::fxtext::findEffect(type);
    if (e == nullptr || knob < 0 || knob >= e->numKnobs || e->knobs[knob].name == nullptr) return nullptr;
    const auto* k = &e->knobs[knob];
    if (k->caseOn < 0) return k;
    const int s = sample1024(switchValue(k->caseOn));
    int c = 0;
    while (c + 1 < k->numCases && k->caseFrom[c + 1] <= s) ++c;
    return &k->cases[c];
}

// ---- stepping: the value `delta` texts away (enum: segment midpoints; else the next sample whose text differs)

float stepThresholds(const std::vector<float>& from, float v, int delta) {
    if (from.empty()) return v;
    int i = 0;
    while (i + 1 < int(from.size()) && from[size_t(i) + 1] <= v) ++i;
    i = std::clamp(i + delta, 0, int(from.size()) - 1);
    const float a = from[size_t(i)], b = i + 1 < int(from.size()) ? from[size_t(i) + 1] : 1.f;
    return std::clamp((a + b) * 0.5f, 0.f, 1.f);
}

float stepWalk(const std::function<std::string(float)>& text, float v, int delta, int res = 1024) {
    int s = std::clamp(int(std::lround(double(v) * res)), 0, res);
    const int dir = delta > 0 ? 1 : -1;
    for (int n = std::abs(delta); n > 0; --n) {
        const std::string t0 = text(float(s) / float(res));
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

float stepPt(const pt::Table& t, float v, int delta) {
    if (t.kind == pt::kEnum) {
        std::vector<float> from;
        for (int i = 0; i < t.numSegs; ++i) from.push_back(t.segs[i].from);
        return stepThresholds(from, v, delta);
    }
    return stepWalk([&t](float x) { return ptText(t, x); }, v, delta);
}

float stepFx(const Knob& k, float v, int delta) {
    std::vector<float> from;
    for (int i = 0; i < k.numSegs; ++i) from.push_back(float(k.segs[i].from) / 1024.f);
    if (from.empty()) return v;
    // the middle of the segment in samples (segment i covers from_i .. from_{i+1} - 1)
    int i = 0;
    const int s = sample1024(v);
    while (i + 1 < k.numSegs && k.segs[i + 1].from <= s) ++i;
    i = std::clamp(i + delta, 0, k.numSegs - 1);
    const int a = k.segs[i].from, b = i + 1 < k.numSegs ? k.segs[i + 1].from - 1 : 1024;
    return float((a + b) / 2) / 1024.f;
}

// ---- generated tables by parameter index

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
};
const TableIndex& tables() {
    static const TableIndex t;
    return t;
}

const char* kTypeNames[] = {"Effect", "Drawbar", "Sample", "Wavetable", "FM", "Analog", "Model"};
std::string typeName(int t) { return t >= 0 && t < 7 ? kTypeNames[t] : "Element"; }

int quant(float v, float k) { return int(v * k); }

// ---- part (VST) parameters: names and tables (plugin/Source/ParamDefs.h kExtDefs + the mixer)

int partParamIndex(int j) {  // engine.cpp: part parameter j -> patch parameter
    static constexpr int8_t kMap[0x30] = {
        -1, -1, 0x43, 0x3c, 0x3d, 0x3f, 0x40, 0x41, 0x42, 0x0e, 0x0f, 0x67, 0x69, 0x6b, 0x6d, 0x31,
        0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3a, 0x3e, -1, 0x3b, 0x49, 0x4a, 0x39, 0x03, -1,
        0x4c, 0x4d, 0x4e, 0x50, 0x51, 0x52, 0x53, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, -1, 0x18, 0x19};
    if (j == 0x1f) return 0x94;
    return j >= 0 && j < 0x30 ? kMap[j] : -1;
}

const char* partParamName(int j) {
    switch (j) {
        case 1: return "Mute";
        case 2: return "Level";
        case 3: return "Pan";
        case 4: return "Output";
        case 5: return "Effect Send 1";
        case 6: return "Effect Send 2";
        case 7: return "Effect Send 3";
        case 8: return "Effect Send 4";
        case 15: return "Key Low";
        case 16: return "Key High";
        case 17: return "Key Crossfade";
        case 18: return "Velocity Low";
        case 19: return "Velocity High";
        case 20: return "Velocity Crossfade";
        case 21: return "Transpose";
        case 22: return "Fine Tune";
        case 23: return "Max Voices";
        case 24: return "Effect Sends";
        case 25: return "Keep Settings";
        case 26: return "Level Trim";
        case 27: return "Pitch Bend Up";
        case 28: return "Pitch Bend Down";
        case 29: return "Mono / Poly";
        case 30: return "Glide";
        case 31: return "Glide Time";
        case 32: return "Tuning 1 Bank";
        case 33: return "Tuning 1 Scale";
        case 34: return "Tuning 1 Root";
        case 35: return "Tuning 2 Bank";
        case 36: return "Tuning 2 Scale";
        case 37: return "Tuning 2 Root";
        case 38: return "Tuning Depth";
        case 39: return "Arpeggiator";
        case 40: return "Arp Latch";
        case 41: return "Arp Mode";
        case 42: return "Arp Octaves";
        case 43: return "Arp Steps";
        case 44: return "Arp Sync";
        case 45: return "Arp Rate";
        case 46: return "Arp Gate";
        case 47: return "Arp Swing";
        default: return nullptr;
    }
}

// global / patch send FX block j (0..23) -> name and table (ParamDefs.h gfxDef)
const char* sendFxName(int j) {
    static const char* const kTail[7] = {"Aux Output", "Level", "Pan", "Send 1", "Send 2", "Send 3", "Send 4"};
    if (j == 0) return "Mute";
    if (j >= 17 && j < 24) return kTail[j - 17];
    return nullptr;  // knobs: the effect's own names
}
const pt::Table* sendFxTable(int j) {
    switch (j) {
        case 0: return &pt::kFxMute;
        case 17: return &pt::kFxAux;
        case 18: return &pt::kFxLevel;
        case 19: return &pt::kFxPan;
        case 20: case 21: case 22: case 23: return &pt::kFxSend;
        default: return nullptr;
    }
}

// ------------------------------------------------------------------------------------------------ mini JSON

struct JVal {
    enum T { Null, Num, Str, Arr, Obj } t = Null;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        for (const auto& [key, v] : o)
            if (key == k) return &v;
        return nullptr;
    }
    double num(const char* k, double def) const {
        const JVal* v = get(k);
        return v && v->t == Num ? v->n : def;
    }
    std::string str(const char* k) const {
        const JVal* v = get(k);
        return v && v->t == Str ? v->s : std::string();
    }
};

struct JParser {
    const char* p;
    const char* e;
    void ws() {
        while (p < e && std::isspace(uint8_t(*p))) ++p;
    }
    bool parse(JVal& v, int depth = 0) {
        if (depth > 32) return false;
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            v.t = JVal::Obj;
            ++p;
            ws();
            if (p < e && *p == '}') return ++p, true;
            while (p < e) {
                JVal k;
                ws();
                if (!parseStr(k.s)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                ++p;
                JVal x;
                if (!parse(x, depth + 1)) return false;
                v.o.emplace_back(std::move(k.s), std::move(x));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') return ++p, true;
                return false;
            }
            return false;
        }
        if (*p == '[') {
            v.t = JVal::Arr;
            ++p;
            ws();
            if (p < e && *p == ']') return ++p, true;
            while (p < e) {
                JVal x;
                if (!parse(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') return ++p, true;
                return false;
            }
            return false;
        }
        if (*p == '"') {
            v.t = JVal::Str;
            return parseStr(v.s);
        }
        if (std::strncmp(p, "null", 4) == 0) return p += 4, v.t = JVal::Null, true;
        if (std::strncmp(p, "true", 4) == 0) return p += 4, v.t = JVal::Num, v.n = 1, true;
        if (std::strncmp(p, "false", 5) == 0) return p += 5, v.t = JVal::Num, v.n = 0, true;
        char* end = nullptr;
        v.n = std::strtod(p, &end);
        if (end == p) return false;
        v.t = JVal::Num;
        p = end;
        return true;
    }
    bool parseStr(std::string& out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                ++p;
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                        if (p + 4 < e) {
                            const int c = int(std::strtol(std::string(p + 1, 4).c_str(), nullptr, 16));
                            out += c < 128 ? char(c) : '?';
                            p += 4;
                        }
                        break;
                    default: out += *p;
                }
                ++p;
            } else {
                out += *p++;
            }
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
};

bool parseJson(const std::string& s, JVal& v) {
    JParser jp{s.data(), s.data() + s.size()};
    return jp.parse(v);
}

// ------------------------------------------------------------------------------------------------ instance

struct BrowseEntry {
    int bank, index;
    std::string name;  // display name (ASCII)
    int bankRow;       // index into Instance::banks
};
struct BankRow {
    int number;
    std::string name;
    int first, count;  // range in browse
};

struct ElemInfo {
    int setter;        // setter element index (record + 1)
    int type;
    std::string name;
};

struct PartInfo {
    bool empty = true;
    int bank = -1, index = -1, browse = -1;
    std::string name, bankName;
    std::array<std::string, kKnobs> knobNames;
    std::vector<ElemInfo> elems;
    std::string warning;
};

// A user change kept for the saved state: re-applied after the patch is loaded again.
using Edits = std::map<std::pair<int, int>, float>;  // (setter element, param) -> value; element -1 = part (VST) j

struct LoadJob {
    int bank = -1, index = -1;
    std::string name;            // to find the patch by name if the numbers moved
    bool restore = false;        // from a saved state: apply edits / clear when not found
    Edits edits;
    uint64_t gen = 0;            // Instance::loadGen at post: only a part's latest request is applied
};
struct FxJob {
    int preset = -2;             // index into lib->effects, -1 = none, -2 = by path
    std::string path;
    bool restore = false;
    std::map<int, float> touch;  // gfx j -> value
};

struct Command {
    enum Kind { loadPatch, globalFx } kind = loadPatch;
    int part = 0;
    std::shared_ptr<hs2::Patch> patch;
    int bank = -1;
    PartInfo info;
    Edits edits;
    // global FX
    int slot = 0;
    float fxType = 1.f;
    std::array<float, 12> knobs{};
    bool empty = false;
    bool touchOnly = false;      // a saved slot that took its effect from the patches: only its touched values
    std::string fxPath;
    std::map<int, float> touch;
    uint64_t gen = 0;            // the load request this command answers
};

struct Settings {
    int midiPc = 1;           // MIDI program changes load patches
    int ringOut = 1;          // effect tails ring out (not in the original; on: delays don't cut off)
    int cutOnLoad = 0;
    int memory = 1;           // 0 ECO, 1 MID, 2 XXL
    float hype = 0.33f, velSens = 0.5f, touchSens = 0.5f;
    int tuneSemis = 0;
    int tuneCents = 0;
    int preload = 1;          // decode a new patch's samples on the loader thread, up to Instance::preloadBudget
};

struct Instance {
    std::string moduleDir;
    fs::path contentRoot;

    std::mutex mu;  // engine, parts, edits, cmds
    std::unique_ptr<hs2::Content> content;
    std::unique_ptr<hs2::Engine> engine;
    std::shared_ptr<const hs2port::PatchLibrary> lib;
    std::vector<BrowseEntry> browse;
    std::vector<BankRow> banks;
    std::vector<std::string> fxLabels;  // per lib->effects
    std::vector<std::string> sampleBankNames = std::vector<std::string>(129), wavetableNames = std::vector<std::string>(129);
    std::array<PartInfo, kParts> parts;
    std::array<int, kParts> channel{{0, 1, 2, 3}};
    std::array<Edits, kParts> edits;     // element / patch edits since the load (setter element >= 0)
    std::array<std::map<int, float>, kParts> touch;  // part VST j -> value set by the user
    std::array<std::map<int, float>, 4> gfxTouch;
    std::array<std::string, 4> gfxPath;  // chosen effect preset (relative to Effects), "" = from the patches
    std::array<int, 4> gfxPreset{{-1, -1, -1, -1}};
    std::optional<float> masterTouch;
    std::array<std::optional<LoadJob>, kParts> restoring;  // a saved part not loaded yet (kept for get state)
    std::array<int, kParts> browseTarget{{-1, -1, -1, -1}};  // the patch asked for last (shown while it loads)
    std::array<int, 4> gfxTarget{{-1, -1, -1, -1}};          // the effect preset asked for last
    std::array<int, kParts> bankMsb{}, bankLsb{};
    std::array<bool, kParts> sawLsb{};
    Settings set;
    int active = 0;
    std::vector<Command> cmds;
    std::vector<hs2::MidiEvent> events;
    std::vector<float> buf;
    bool ready = false;
    int scanState = 0;  // 0 scanning, 1 ready, -1 no content
    bool everRestored = false;  // a state was applied (a fresh instance loads default patches instead)
    // Diagnostics in Schwung's debug log (rate limited): incoming MIDI and its routing, then the output level.
    int midiLogLeft = 40, levelLogLeft = 0, levelBlocks = 0;
    float levelPeak = 0;
    std::string pendingState;

    // worker
    std::thread worker;
    std::mutex qmu;
    std::condition_variable cv;
    bool quit = false;
    bool scanRequested = true;
    std::array<std::optional<LoadJob>, kParts> jobs;
    std::array<uint64_t, kParts> loadGen{};  // guarded by qmu
    std::array<std::atomic<uint64_t>, kParts> doneGen{};  // the last request applied or failed (loading = different)
    std::array<std::optional<FxJob>, 4> fxJobs;
    std::string status = "Scanning content...";
    std::string error;
    // Decoded samples: preloads stop when the cache holds this much (later notes still decode on first use). The
    // cache only keeps what a part can still play: releaseUnusedSamples drops the rest after loads.
    size_t preloadBudget = size_t(256) << 20;
    bool sweepDue = false;  // worker only: a load finished, release the samples nothing uses once it is applied
    // Worker only: loads whose samples still need decoding (the load's throwaway player, which reads the patch's
    // zones). They run when no load is queued, so four parts loading at once each get their patch first; a newer load
    // for the part drops its entry. Declared after content: destroyed before it.
    struct Preload {
        std::unique_ptr<hs2::PatchPlayer> player;
        uint64_t gen = 0;
        std::string label;
    };
    std::array<std::optional<Preload>, kParts> preloads;

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(qmu);
        status = s;
    }
    std::string getStatus() {
        std::lock_guard<std::mutex> lk(qmu);
        return status;
    }
    void post(int part, LoadJob j) {
        {
            std::lock_guard<std::mutex> lk(qmu);
            j.gen = ++loadGen[size_t(part)];
            jobs[size_t(part)] = std::move(j);
        }
        cv.notify_one();
    }
    void postFx(int k, FxJob j) {
        {
            std::lock_guard<std::mutex> lk(qmu);
            fxJobs[size_t(k)] = std::move(j);
        }
        cv.notify_one();
    }

    void workerLoop();
    void scan();
    void runLoad(int part, LoadJob job);
    void runFx(int k, FxJob job);
    bool releaseUnusedSamples();   // worker; false = deferred (a load is still waiting for the audio thread)
    void runPreloads();            // worker
    void applyCommands();          // audio thread, mu held
    void applySettings();          // mu held
    std::string stateJson();       // mu held
    void restoreState(const std::string& s);  // mu not held
};

float memoryValue(int m) { return m == 0 ? 0.f : m == 1 ? 0.5f : 1.f; }

void Instance::applySettings() {
    if (!engine) return;
    engine->setRingOut(set.ringOut != 0);
    engine->setCutOnLoad(set.cutOnLoad != 0);
    engine->setHype(set.hype);
    engine->setVelSens(set.velSens);
    engine->setTouchSens(set.touchSens);
    engine->setMasterTune(set.tuneSemis, float(set.tuneCents) / 100.f);
}

void Instance::scan() {
    auto t0 = std::chrono::steady_clock::now();
    fs::path root = contentRoot;
    std::error_code ec;
    if (!fs::is_directory(root / "Patches", ec)) {
        // also accept the folder copied with its own name inside content/
        for (const auto& e : fs::directory_iterator(root, ec))
            if (e.is_directory() && fs::is_directory(e.path() / "Patches", ec)) {
                root = e.path();
                break;
            }
    }
    if (!fs::is_directory(root / "Patches", ec)) {
        std::lock_guard<std::mutex> lk(mu);
        scanState = -1;
        setStatus("Hypersonic 2 content not found. Copy the content folder to " + contentRoot.string());
        logf("content not found under %s", contentRoot.c_str());
        return;
    }
    auto content_ = std::make_unique<hs2::Content>(root);
    auto engine_ = std::make_unique<hs2::Engine>(*content_, float(g_host ? g_host->sample_rate : 44100));
    auto lib_ = std::make_shared<hs2port::PatchLibrary>(hs2port::PatchLibrary::scan(root));
    std::vector<BrowseEntry> browse_;
    std::vector<BankRow> banks_;
    for (const auto& b : lib_->banks) {
        if (b.combi) continue;  // combis are 16-part setups; this build has four parts
        BankRow row{b.number, ascii(b.name), int(browse_.size()), 0};
        for (int pi : b.patches) {
            const auto& p = lib_->patches[size_t(pi)];
            if (p.copyProtected && !p.captured) continue;  // no capture: cannot be loaded
            std::string n = ascii(hs2port::displayName(p.name));
            if (n.empty()) n = "Patch " + std::to_string(p.index + 1);
            browse_.push_back({p.bank, p.index, n, int(banks_.size())});
            ++row.count;
        }
        if (row.count > 0) banks_.push_back(row);
    }
    std::vector<std::string> fxl;
    for (const auto& fx : lib_->effects) {
        std::string s = fx.rel.generic_string();
        if (s.size() > 4 && s.substr(s.size() - 4) == ".fxp") s.resize(s.size() - 4);
        fxl.push_back(ascii(s));
    }
    std::vector<std::string> sbn(129), wtn(129);
    auto names = [&](const char* dir, std::vector<std::string>& out) {
        for (const auto& e : fs::directory_iterator(root / dir, ec)) {
            const auto n = e.path().filename().string();
            if (n.size() >= 4 && std::isdigit(uint8_t(n[0])) && std::isdigit(uint8_t(n[1])) &&
                std::isdigit(uint8_t(n[2])) && n[3] == ' ') {
                const int k = std::stoi(n.substr(0, 3));
                if (k >= 0 && k <= 128) out[size_t(k)] = ascii(n);
            }
        }
    };
    names("Samples", sbn);
    names("Wavetables", wtn);
    for (const auto& err : lib_->errors) logf("library: %s", err.c_str());
    {
        std::lock_guard<std::mutex> lk(mu);
        // A rescan: the current setup is reloaded from the new scan (by bank number and name).
        if (engine && pendingState.empty()) pendingState = stateJson();
        cmds.clear();
        engine.reset();  // before the content it reads from
        content = std::move(content_);
        engine = std::move(engine_);
        parts = {};
        for (auto& e : edits) e.clear();
        for (auto& t : touch) t.clear();
        for (auto& t : gfxTouch) t.clear();
        for (auto& r : restoring) r.reset();
        browseTarget = {{-1, -1, -1, -1}};
        gfxTarget = {{-1, -1, -1, -1}};
        gfxPath = {};
        gfxPreset = {{-1, -1, -1, -1}};
        masterTouch.reset();
        if (set.memory != 1) engine->setMemoryMode(memoryValue(set.memory));
        applySettings();
        lib = lib_;
        browse = std::move(browse_);
        banks = std::move(banks_);
        fxLabels = std::move(fxl);
        sampleBankNames = std::move(sbn);
        wavetableNames = std::move(wtn);
        scanState = 1;
        ready = true;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logf("content %s: %zu patches in %zu banks, %zu effect presets (%.0f ms)", root.c_str(), browse.size(),
         banks.size(), fxLabels.size(), ms);
    setStatus("Ready. " + std::to_string(browse.size()) + " patches.");
}

void Instance::runLoad(int part, LoadJob job) {
    std::shared_ptr<const hs2port::PatchLibrary> L;
    hs2::Content* C;
    float sr;
    float mem;
    int preload;
    {
        std::lock_guard<std::mutex> lk(mu);
        L = lib;
        C = content.get();
        sr = float(g_host ? g_host->sample_rate : 44100);
        mem = memoryValue(set.memory);
        preload = set.preload;
    }
    if (!L || !C) return;
    int index = job.index;
    if (!job.name.empty()) {  // the bank may have changed since the state was saved
        const int i = L->find(job.bank, index);
        if (i < 0 || ascii(hs2port::displayName(L->patches[size_t(i)].name)) != job.name) {
            for (const auto& p : L->patches)
                if (p.bank == job.bank && ascii(hs2port::displayName(p.name)) == job.name) {
                    index = p.index;
                    break;
                }
        }
    }
    const std::string partName = "Part " + std::to_string(part + 1);
    try {
        const auto path = C->patchBankPath(job.bank);
        if (path.empty()) throw std::runtime_error("bank " + std::to_string(job.bank) + " is not in the content folder");
        auto mb = hs2::MilanBank::open(path);
        if (mb->kind != 1) throw std::runtime_error("bank " + std::to_string(job.bank) + " is not a patch bank");
        if (index < 0 || size_t(index) >= mb->entries.size() || mb->entries[size_t(index)].slot1.size == 0)
            throw std::runtime_error("patch " + std::to_string(index + 1) + " does not exist in this bank");
        const auto data = mb->read(mb->entries[size_t(index)].slot1);
        // A copy-protected patch: its capture, the plain patch hs2capture (tools/hs2capture, --mode patch)
        // took out of the licensed original, which PatchLibrary::scan found in <content>/Captures.
        std::vector<uint8_t> blob;
        if (hs2port::isCopyProtectedPatch(data.data(), data.size())) {
            const auto* cap = L->capturePath(job.bank, index);
            if (cap == nullptr) throw std::runtime_error("it is copy protected and has no capture (hs2capture)");
            std::ifstream f(*cap, std::ios::binary);
            if (!f) throw std::runtime_error("its capture file cannot be read");
            blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        } else {
            blob.assign(data.begin(), data.end());
        }
        auto patch = std::make_shared<hs2::Patch>(hs2::Patch::parse(blob));
        // A throwaway player reads the sample banks / wavetables into the shared caches, so the engine's own load on
        // the audio thread finds them there (as the plugin's loader does).
        auto tmp = std::make_unique<hs2::PatchPlayer>(*C, sr);
        tmp->setMemoryMode(mem);
        tmp->load(*patch);
        const std::vector<std::string> warnings = tmp->warnings();
        Command cmd;
        cmd.kind = Command::loadPatch;
        cmd.part = part;
        cmd.bank = job.bank;
        cmd.patch = patch;
        cmd.edits = std::move(job.edits);
        cmd.gen = job.gen;
        PartInfo& inf = cmd.info;
        inf.empty = false;
        inf.bank = job.bank;
        inf.index = index;
        inf.name = ascii(hs2port::displayName(patch->name));
        if (const int li = L->find(job.bank, index); li >= 0) {
            const auto n = ascii(hs2port::displayName(L->patches[size_t(li)].name));
            if (!n.empty()) inf.name = n;
        }
        if (inf.name.empty()) inf.name = "Untitled patch";
        if (const auto* b = L->bank(job.bank)) inf.bankName = ascii(b->name);
        for (int k = 0; k < kKnobs; ++k) inf.knobNames[size_t(k)] = ascii(patch->quickControls[size_t(k)]);
        for (size_t r = 0; r < patch->elements.size(); ++r) {
            const auto& rec = patch->elements[r];
            inf.elems.push_back({int(r) + 1, int(hs2::elementType(rec)), ascii(rec.name)});
        }
        for (const auto& w : warnings) inf.warning += (inf.warning.empty() ? "" : "; ") + w;
        const std::string shownName = inf.name, warningText = inf.warning;  // inf moves into cmds below
        {
            std::lock_guard<std::mutex> lk(mu);
            {
                std::lock_guard<std::mutex> q(qmu);
                if (job.gen != loadGen[size_t(part)]) return;  // a newer request for this part (a restore, a scroll)
            }
            for (size_t i = 0; i < browse.size(); ++i)
                if (browse[i].bank == job.bank && browse[i].index == index) {
                    inf.browse = int(i);
                    break;
                }
            restoring[size_t(part)].reset();
            cmds.push_back(std::move(cmd));
        }
        setStatus(partName + ": " + shownName);
        if (!warnings.empty()) logf("%s %s: not supported yet: %s", partName.c_str(), shownName.c_str(), warningText.c_str());
        // The patch plays already; its samples decode on this thread instead of on the audio thread at the first notes
        // (runPreloads, once no other load waits).
        if (preload) preloads[size_t(part)] = Preload{std::move(tmp), job.gen, partName + " " + shownName};
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lk(mu);
            std::lock_guard<std::mutex> q(qmu);
            if (job.gen != loadGen[size_t(part)]) return;
            restoring[size_t(part)].reset();
            doneGen[size_t(part)] = job.gen;
        }
        setStatus(partName + ": could not load patch " + std::to_string(index + 1) + " of bank " +
                  std::to_string(job.bank) + ": " + e.what());
        logf("%s", getStatus().c_str());
    }
}


void Instance::runFx(int k, FxJob job) {
    std::shared_ptr<const hs2port::PatchLibrary> L;
    fs::path root;
    {
        std::lock_guard<std::mutex> lk(mu);
        L = lib;
        root = content ? content->root() : contentRoot;
    }
    if (!L) return;
    Command cmd;
    cmd.kind = Command::globalFx;
    cmd.slot = k;
    cmd.touch = std::move(job.touch);
    int preset = job.preset;
    if (preset == -2) {  // a saved slot, by its preset path
        if (job.path.empty()) {
            cmd.touchOnly = true;
        } else {
            preset = -1;
            for (size_t i = 0; i < L->effects.size(); ++i)
                if (L->effects[i].rel.generic_string() == job.path) preset = int(i);
            if (preset < 0) {
                setStatus("Effect " + std::to_string(k + 1) + ": preset not found: " + job.path);
                cmd.touchOnly = true;
            }
        }
    }
    std::string label = "from the patches";
    if (!cmd.touchOnly) {
        if (preset < 0 || preset >= int(L->effects.size())) {
            cmd.empty = true;
            label = "none";
        } else {
            hs2port::EffectPreset fx;
            if (!hs2port::EffectPreset::read(root / "Effects" / L->effects[size_t(preset)].rel, fx)) {
                setStatus("Effect " + std::to_string(k + 1) + ": could not read the preset");
                return;
            }
            cmd.fxType = fx.type;
            cmd.knobs = fx.knobs;
            cmd.fxPath = L->effects[size_t(preset)].rel.generic_string();
            label = ascii(cmd.fxPath);
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!cmd.touchOnly) gfxPreset[size_t(k)] = cmd.empty ? -1 : preset;
        cmds.push_back(std::move(cmd));
    }
    setStatus("Effect " + std::to_string(k + 1) + ": " + label);
}

void Instance::workerLoop() {
    for (;;) {
        bool doScan = false;
        int part = -1, fxk = -1;
        LoadJob lj;
        FxJob fj;
        {
            std::unique_lock<std::mutex> lk(qmu);
            const auto queued = [this] {
                if (quit || scanRequested) return true;
                for (const auto& j : jobs)
                    if (j) return true;
                for (const auto& j : fxJobs)
                    if (j) return true;
                return false;
            };
            bool preloadsDue = false;
            for (const auto& p : preloads) preloadsDue = preloadsDue || p.has_value();
            // idle: preloads first, then a sweep soon after a load (once it is applied) and every 2 s (sounding notes
            // of replaced patches end)
            const bool woke = preloadsDue ? queued()
                                          : cv.wait_for(lk, std::chrono::milliseconds(sweepDue ? 100 : 2000), queued);
            if (quit) return;
            if (!woke) {
                lk.unlock();
                if (preloadsDue) runPreloads();
                else if (releaseUnusedSamples()) sweepDue = false;
                continue;
            }
            if (scanRequested) {
                scanRequested = false;
                doScan = true;
            } else {
                for (int p = 0; p < kParts && part < 0; ++p)
                    if (jobs[size_t(p)]) {
                        part = p;
                        lj = std::move(*jobs[size_t(p)]);
                        jobs[size_t(p)].reset();
                    }
                if (part < 0)  // effect presets after the patches (a restore loads the patches first)
                    for (int k = 0; k < 4 && fxk < 0; ++k)
                        if (fxJobs[size_t(k)]) {
                            fxk = k;
                            fj = std::move(*fxJobs[size_t(k)]);
                            fxJobs[size_t(k)].reset();
                        }
            }
        }
        if (doScan) {
            for (auto& p : preloads) p.reset();  // their players read the old content
            scan();
            std::string st;
            {
                std::lock_guard<std::mutex> lk(mu);
                st.swap(pendingState);
            }
            if (!st.empty()) {
                restoreState(st);
            } else {  // a fresh instance: a patch in every part, so every track plays at once
                std::vector<std::pair<int, LoadJob>> defaults;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (!everRestored && !browse.empty())
                        for (int p = 0; p < kParts; ++p)
                            if (parts[size_t(p)].empty && browseTarget[size_t(p)] < 0) {
                                const int b = std::min(p, int(browse.size()) - 1);
                                browseTarget[size_t(p)] = b;
                                LoadJob j;
                                j.bank = browse[size_t(b)].bank;
                                j.index = browse[size_t(b)].index;
                                defaults.push_back({p, std::move(j)});
                            }
                }
                for (auto& [p, j] : defaults) post(p, std::move(j));
            }
            continue;
        }
        if (part >= 0) {
            runLoad(part, std::move(lj));
            sweepDue = true;
        } else if (fxk >= 0) {
            runFx(fxk, std::move(fj));
        }
    }
}

void Instance::runPreloads() {
    for (int p = 0; p < kParts; ++p) {
        auto& pl = preloads[size_t(p)];
        if (!pl) continue;
        {
            std::lock_guard<std::mutex> q(qmu);
            if (pl->gen != loadGen[size_t(p)]) {  // a newer load for this part
                pl.reset();
                continue;
            }
        }
        hs2::Content* C;
        {
            std::lock_guard<std::mutex> lk(mu);
            C = content.get();
        }
        const auto t0 = std::chrono::steady_clock::now();
        bool interrupted = false, full = false;
        const size_t got = pl->player->preloadSamples([&] {
            {
                std::lock_guard<std::mutex> q(qmu);
                if (quit || scanRequested) return !(interrupted = true);
                for (const auto& j : jobs)
                    if (j) return !(interrupted = true);
            }
            if (C->decodedSampleBytes() >= preloadBudget) return !(full = true);
            return true;
        });
        if (interrupted) return;  // a load waits: it goes first, this preload resumes after it
        if (got)
            logf("%s: %.1f MB of samples ready%s (%.0f ms, %.1f MB decoded in all)", pl->label.c_str(),
                 double(got) / 1048576.0, full ? ", stopped at the preload budget" : "",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                 double(C->decodedSampleBytes()) / 1048576.0);
        pl.reset();
    }
}

// Frees the decoded samples no player can read any more: not in a zone of a loaded, pending, still sounding or
// fading part, and held by no voice. Under mu render cannot run, and the only other reader of the sample caches
// (a load's throwaway player) runs on this thread. The memory itself is freed after mu is released.
bool Instance::releaseUnusedSamples() {
    std::vector<std::unique_ptr<hs2::SampleData>> dead;
    size_t freed = 0, held = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!content || !engine) return true;
        if (!cmds.empty()) return false;  // a prepared load whose samples are not in the engine yet
        hs2::PatchPlayer::SampleRefs refs;
        engine->collectSampleRefs(refs);
        std::sort(refs.zones.begin(), refs.zones.end());
        const std::unordered_set<const hs2::SampleData*> playing(refs.voices.begin(), refs.voices.end());
        freed = content->releaseSamples(
            [&](const hs2::SampleBank* b, uint32_t i, const hs2::SampleData* sd) {
                return playing.count(sd) || std::binary_search(refs.zones.begin(), refs.zones.end(),
                                                               std::pair<const hs2::SampleBank*, uint32_t>{b, i});
            },
            dead);
        if (freed) held = content->decodedSampleBytes();
    }
    dead.clear();
    if (freed) logf("samples: released %.1f MB, %.1f MB still decoded", double(freed) / 1048576.0, double(held) / 1048576.0);
    return true;
}

// Audio thread, mu held: the loads the worker prepared.
void Instance::applyCommands() {
    for (auto& c : cmds) {
        if (c.kind == Command::loadPatch) {
            const int p = c.part;
            engine->loadPatch(p, *c.patch, c.bank);
            doneGen[size_t(p)] = c.gen;
            parts[size_t(p)] = std::move(c.info);
            browseTarget[size_t(p)] = parts[size_t(p)].browse;
            edits[size_t(p)].clear();
            // the part settings a load keeps (Mute, Level, ...) stay in the saved state; the others came from the patch
            for (auto it = touch[size_t(p)].begin(); it != touch[size_t(p)].end();)
                it = engine->getParameter(p * kPP + it->first) != it->second ? touch[size_t(p)].erase(it) : std::next(it);
            for (const auto& [key, v] : c.edits) {
                if (key.first < 0) {
                    engine->setParameter(p * kPP + key.second, v);
                    touch[size_t(p)][key.second] = v;
                } else {
                    engine->setPartSetter(p, key.first, key.second, v);
                    edits[size_t(p)][key] = v;
                }
            }
        } else {
            const int k = c.slot;
            if (!c.touchOnly) {
                engine->loadGlobalFx(k, c.fxType, c.empty ? nullptr : c.knobs.data());
                gfxPath[size_t(k)] = c.empty ? std::string("none") : c.fxPath;
                gfxTouch[size_t(k)].clear();
            }
            for (const auto& [j, v] : c.touch) {
                engine->setParameter(hs2::Engine::kGlobalFxBase + k * 0x18 + j, v);
                gfxTouch[size_t(k)][j] = v;
            }
        }
    }
    cmds.clear();
}

// ------------------------------------------------------------------------------------------------ addresses

// M            master volume
// P<p>.<j>     part p's engine (VST) parameter j: mixer, Hyperknobs (9..14), settings, arp, tuning, send FX (56..)
// G<k>.<j>     global send effect k's parameter j (0 mute, 1..16 knobs, 17 aux, 18 level, 19 pan, 20..23 sends)
// X<p>.<i>     part p's patch parameter i
// E<p>.<e>.<i> part p's element e (setter index = record + 1) parameter i
// C<p>         part p's MIDI channel        B<p> part p's patch (browse)     K<p> part p's bank (browse by bank)
// F<k>         global effect k's preset     Z<n> module setting n            A    the active part
struct Addr {
    char k = 0;
    int p = 0, e = 0, i = 0;
};

bool parseAddr(const char* s, Addr& a) {
    if (!s || !*s) return false;
    a.k = s[0];
    int nums[3] = {0, 0, 0}, n = 0;
    const char* q = s + 1;
    while (*q && n < 3) {
        char* end = nullptr;
        const long v = std::strtol(q, &end, 10);
        if (end == q) return false;
        nums[n++] = int(v);
        q = end;
        if (*q == '.') ++q;
        else if (*q) return false;
    }
    auto part = [](int p) { return p >= 0 && p < kParts; };
    switch (a.k) {
        case 'M': case 'A': return n == 0;
        case 'P': a.p = nums[0], a.i = nums[1]; return n == 2 && part(a.p) && a.i >= 1 && a.i < 56 + 4 * 24;
        case 'G': a.p = nums[0], a.i = nums[1]; return n == 2 && a.p >= 0 && a.p < 4 && a.i >= 0 && a.i < 24;
        case 'X': a.p = nums[0], a.i = nums[1]; return n == 2 && part(a.p) && a.i >= 0 && a.i < 0xfc;
        case 'E': a.p = nums[0], a.e = nums[1], a.i = nums[2]; return n == 3 && part(a.p) && a.e >= 1 && a.e <= 0x40 && a.i >= 0 && a.i < 256;
        case 'C': case 'B': case 'K': a.p = nums[0]; return n == 1 && part(a.p);
        case 'F': a.p = nums[0]; return n == 1 && a.p >= 0 && a.p < 4;
        case 'Z': a.i = nums[0]; return n == 1 && a.i >= 0 && a.i < 10;
        default: return false;
    }
}

const char* kSettingNames[10] = {"MIDI Program Change", "Ring Out Effect Tails", "Cut Notes on Load", "Memory Mode",
                                 "Hype", "Velocity Sensitivity", "Aftertouch Sensitivity", "Master Tune",
                                 "Master Fine Tune", "Preload Samples"};

std::string onOff(bool on) { return on ? "on" : "off"; }
std::string noteName(int n) {  // Hypersonic's convention: note 0 = C-2, 60 = C3
    static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    n = std::clamp(n, 0, 127);
    return std::string(kNames[n % 12]) + std::to_string(n / 12 - 2);
}
std::string signedNum(int n, const char* unit) {
    return (n > 0 ? "+" : "") + std::to_string(n) + unit;
}

struct Api {  // everything below runs with Instance::mu held
    Instance& I;
    hs2::Engine& E() { return *I.engine; }

    int elemType(int p, int e) {
        for (const auto& el : I.parts[size_t(p)].elems)
            if (el.setter == e) return el.type;
        return -1;
    }

    // The type of the effect a send / global slot holds (128 = none).
    int partSendType(int p, int s) { return quant(E().getPartSetter(p, 0x41 + s, 0x13), 128.9f); }

    const Knob* sendKnob(int type, int knob, std::function<float(int)> sw) {
        if (type >= 128) return nullptr;
        return fxKnob(type, knob, sw);
    }
    const Knob* gfxKnob(int k, int knob) {
        return sendKnob(E().globalFxType(k), knob, [this, k](int sw) {
            return E().getParameter(hs2::Engine::kGlobalFxBase + k * 0x18 + 1 + sw);
        });
    }
    const Knob* psendKnob(int p, int s, int knob) {
        return sendKnob(partSendType(p, s), knob, [this, p, s](int sw) {
            return E().getParameter(p * kPP + 56 + s * 24 + 1 + sw);
        });
    }
    const Knob* elemKnob(int p, int e, int knob) {
        const int type = quant(E().getPartSetter(p, e, 0x13), 128.9f);
        return sendKnob(type, knob, [this, p, e](int sw) { return E().getPartSetter(p, e, 0x19 + sw); });
    }

    // ---- element parameter names (nullptr = not shown)
    std::string elemName(int p, int e, int i) {
        const int t = elemType(p, e);
        if (t == 0 && i >= 0x19 && i <= 0x24) {
            const Knob* k = elemKnob(p, e, i - 0x19);
            return k ? asciiUtf8(k->name) : std::string();
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
        if (!tp || !tp->name) return {};
        std::string n = trim(tp->name);
        if (n.find("(test only)") != std::string::npos) return {};
        if (const auto sl = n.find(" / "); sl != std::string::npos) n = trim(t == 2 ? n.substr(sl + 3) : n.substr(0, sl));
        else if (n.size() > 2 && n.substr(n.size() - 2) == " /") n = t == 2 ? std::string() : trim(n.substr(0, n.size() - 2));
        if (t == 1 && i >= 0x19 && i <= 0x21) n = "Drawbar " + n + "'";
        if (i >= 0xac && i < 0xfc) {  // "Mod 3 Src" -> "Source" (the page names the row)
            static const char* const kField[5] = {"Source", "Modifier", "Mod Parameter", "Destination", "Depth"};
            n = kField[(i - 0xac) % 5];
        }
        return n;
    }

    // ---- patch parameter names
    std::string patchName(int i) {
        const tb::Param* tp = tables().patch[size_t(i)];
        if (!tp || !tp->name) return {};
        std::string n = trim(tp->name);
        if (n.empty() || n[0] == '<') return {};
        if (n == "ofs" || n == "rng") {  // the knob range rows: name from the knob entry before them
            for (int b = i - 1; b >= 0 && b >= i - 2; --b)
                if (const tb::Param* kp = tables().patch[size_t(b)]; kp && kp->name && std::strncmp(kp->name, "Knob", 4) == 0)
                    return trim(kp->name) + (n == "ofs" ? " Offset" : " Range");
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

    // ---- names
    std::string name(const Addr& a) {
        switch (a.k) {
            case 'M': return "Master Volume";
            case 'A': return "Active Part";
            case 'C': return "Part " + std::to_string(a.p + 1) + " MIDI Channel";
            case 'B': return "Part " + std::to_string(a.p + 1) + " Patch";
            case 'K': return "Part " + std::to_string(a.p + 1) + " Bank";
            case 'F': return "Effect " + std::to_string(a.p + 1) + " Preset";
            case 'Z': return kSettingNames[a.i];
            case 'G': {
                const std::string pre = "Effect " + std::to_string(a.p + 1) + " ";
                if (a.i >= 1 && a.i <= 16) {
                    const Knob* k = gfxKnob(a.p, a.i - 1);
                    return k ? pre + asciiUtf8(k->name) : std::string();
                }
                return pre + sendFxName(a.i);
            }
            case 'P': {
                const int j = a.i;
                if (j >= 9 && j <= 14) {
                    const auto& n = I.parts[size_t(a.p)].knobNames[size_t(j - 9)];
                    return "Hyperknob " + std::to_string(j - 8) + (n.empty() ? std::string() : ": " + n);
                }
                if (j >= 56) {
                    const int s = (j - 56) / 24, jj = (j - 56) % 24;
                    const std::string pre = "Send FX " + std::to_string(s + 1) + " ";
                    if (jj >= 1 && jj <= 16) {
                        const Knob* k = psendKnob(a.p, s, jj - 1);
                        return k ? pre + asciiUtf8(k->name) : std::string();
                    }
                    return pre + sendFxName(jj);
                }
                const char* n = partParamName(j);
                return n ? n : std::string();
            }
            case 'X': return patchName(a.i);
            case 'E': return elemName(a.p, a.e, a.i);
        }
        return {};
    }

    // ---- values
    float value(const Addr& a) {
        switch (a.k) {
            case 'M': return E().getParameter(0);
            case 'A': return float(I.active);
            case 'P': return E().getParameter(a.p * kPP + a.i);
            case 'G': return E().getParameter(hs2::Engine::kGlobalFxBase + a.p * 0x18 + a.i);
            case 'X': return E().getPartSetter(a.p, 0, a.i);
            case 'E': return E().getPartSetter(a.p, a.e, a.i);
            case 'C': return float(I.channel[size_t(a.p)]);
            case 'B': return float(I.browseTarget[size_t(a.p)]);
            case 'K': {
                const int b = I.browseTarget[size_t(a.p)];
                return b >= 0 && b < int(I.browse.size()) ? float(I.browse[size_t(b)].bankRow) : -1.f;
            }
            case 'F': return float(I.gfxTarget[size_t(a.p)]);
            case 'Z': {
                const auto& s = I.set;
                const float v[10] = {float(s.midiPc), float(s.ringOut), float(s.cutOnLoad), float(s.memory), s.hype,
                                     s.velSens, s.touchSens, float(s.tuneSemis), float(s.tuneCents), float(s.preload)};
                return v[a.i];
            }
        }
        return 0;
    }

    void setValue(const Addr& a, float v) {
        switch (a.k) {
            case 'M': E().setParameter(0, v); I.masterTouch = v; return;
            case 'A': I.active = std::clamp(int(v), 0, kParts - 1); return;
            case 'P': E().setParameter(a.p * kPP + a.i, v); I.touch[size_t(a.p)][a.i] = v; return;
            case 'G':
                E().setParameter(hs2::Engine::kGlobalFxBase + a.p * 0x18 + a.i, v);
                I.gfxTouch[size_t(a.p)][a.i] = v;
                return;
            case 'X':
                E().setPartSetter(a.p, 0, a.i, v);
                I.edits[size_t(a.p)][{0, a.i}] = v;
                return;
            case 'E':
                if (elemType(a.p, a.e) < 0) return;
                E().setPartSetter(a.p, a.e, a.i, v);
                I.edits[size_t(a.p)][{a.e, a.i}] = v;
                return;
            case 'C': I.channel[size_t(a.p)] = std::clamp(int(v), 0, kAllChannels); return;
            case 'B': loadBrowse(a.p, int(v)); return;
            case 'K': {
                const int r = std::clamp(int(v), 0, int(I.banks.size()) - 1);
                if (r >= 0) loadBrowse(a.p, I.banks[size_t(r)].first);
                return;
            }
            case 'F': loadFx(a.p, int(v)); return;
            case 'Z': {
                auto& s = I.set;
                switch (a.i) {
                    case 0: s.midiPc = v > 0.5f; break;
                    case 1: s.ringOut = v > 0.5f; break;
                    case 2: s.cutOnLoad = v > 0.5f; break;
                    case 3:
                        s.memory = std::clamp(int(v), 0, 2);
                        E().setMemoryMode(memoryValue(s.memory));
                        break;
                    case 4: s.hype = std::clamp(v, 0.f, 1.f); break;
                    case 5: s.velSens = std::clamp(v, 0.f, 1.f); break;
                    case 6: s.touchSens = std::clamp(v, 0.f, 1.f); break;
                    case 7: s.tuneSemis = std::clamp(int(std::lround(v)), -12, 12); break;
                    case 8: s.tuneCents = std::clamp(int(std::lround(v)), -50, 50); break;
                    case 9: s.preload = v > 0.5f; break;
                }
                I.applySettings();
                return;
            }
        }
    }

    void loadBrowse(int p, int idx) {
        if (I.browse.empty()) return;
        idx = std::clamp(idx, 0, int(I.browse.size()) - 1);
        I.browseTarget[size_t(p)] = idx;
        const auto& b = I.browse[size_t(idx)];
        LoadJob j;
        j.bank = b.bank;
        j.index = b.index;
        I.post(p, std::move(j));
    }
    void loadFx(int k, int preset) {
        const int n = I.lib ? int(I.lib->effects.size()) : 0;
        preset = std::clamp(preset, -1, n - 1);
        I.gfxTarget[size_t(k)] = preset;
        FxJob j;
        j.preset = preset;
        I.postFx(k, std::move(j));
    }

    // ---- texts
    std::string sendText(int jj, float v, const Knob* k, int type) {
        if (jj >= 1 && jj <= 16) return k ? fxText(*k, v) : (type >= 128 ? "no effect" : "unused");
        const pt::Table* t = sendFxTable(jj);
        return t ? ptText(*t, v) : percent(v);
    }

    std::string tuningBankText(float v) {
        const int n = quant(v, 128.9f);
        if (const auto* t = I.lib ? I.lib->tuning(n) : nullptr) return ascii(t->name);
        return "none";
    }
    std::string tuningScaleText(int p, int bankJ, float v) {
        const int n = quant(E().getParameter(p * kPP + bankJ), 128.9f), idx = quant(v, 128.9f);
        if (const auto* t = I.lib ? I.lib->tuning(n) : nullptr)
            return idx >= 0 && idx < int(t->scales.size()) ? ascii(t->scales[size_t(idx)]) : std::string("none");
        return "none";
    }

    std::string sampleWaveText(int bank, int w) {
        hs2::SampleBank* sb = I.content ? I.content->sampleBank(bank) : nullptr;
        if (sb && w >= 0 && w < int(sb->instruments.size())) {
            const auto n = ascii(sb->instruments[size_t(w)].name);
            return std::to_string(w + 1) + " " + (n.empty() ? std::string("(unnamed)") : n);
        }
        return std::to_string(w + 1) + " (none)";
    }

    std::string text(const Addr& a) {
        const float v = value(a);
        switch (a.k) {
            case 'M': {
                const double scale = (8.0 - 2.2 * double(I.set.hype)) / 4.0;
                return gainText(scale * double(v) * double(v));
            }
            case 'A': return "Part " + std::to_string(I.active + 1);
            case 'C': {
                const int c = I.channel[size_t(a.p)];
                return c >= kAllChannels ? std::string("all channels") : "channel " + std::to_string(c + 1);
            }
            case 'B': {
                const int b = I.browseTarget[size_t(a.p)];
                if (b < 0 || b >= int(I.browse.size())) return I.parts[size_t(a.p)].empty ? "empty" : I.parts[size_t(a.p)].name;
                return I.browse[size_t(b)].name;
            }
            case 'K': {
                const int r = int(v);
                return r >= 0 && r < int(I.banks.size()) ? I.banks[size_t(r)].name : std::string("none");
            }
            case 'F': {
                const int f = I.gfxTarget[size_t(a.p)];
                if (f >= 0 && f < int(I.fxLabels.size())) return I.fxLabels[size_t(f)];
                const int type = E().globalFxType(a.p);
                if (type < 128) {
                    const char* n = hs2::effectName(type);
                    return std::string(n ? n : "effect") + " (from the patch)";
                }
                return "none";
            }
            case 'Z':
                switch (a.i) {
                    case 0: case 1: case 2: case 9: return onOff(v > 0.5f);
                    case 3: { static const char* const m[3] = {"ECO", "MID", "XXL"}; return m[std::clamp(int(v), 0, 2)]; }
                    case 7: return signedNum(int(v), " semitones");
                    case 8: return signedNum(int(v), " cents");
                    default: return percent(v);
                }
            case 'G': {
                const Knob* k = a.i >= 1 && a.i <= 16 ? gfxKnob(a.p, a.i - 1) : nullptr;
                return sendText(a.i, v, k, E().globalFxType(a.p));
            }
            case 'P': {
                const int j = a.i;
                if (j >= 56) {
                    const int s = (j - 56) / 24, jj = (j - 56) % 24;
                    const Knob* k = jj >= 1 && jj <= 16 ? psendKnob(a.p, s, jj - 1) : nullptr;
                    return sendText(jj, v, k, partSendType(a.p, s));
                }
                switch (j) {
                    case 1: return v > 0.5f ? "muted" : "playing";
                    case 2: case 5: case 6: case 7: case 8:
                        return v <= 0.f ? std::string("off") : percent(v) + ", " + gainText(double(v) * double(v));
                    case 3: {
                        const int pn = int(std::lround((double(v) - 0.5) * 200.0));
                        return pn == 0 ? std::string("centre")
                                       : (pn < 0 ? "left " + std::to_string(-pn) : "right " + std::to_string(pn));
                    }
                    case 9: case 10: case 11: case 12: case 13: case 14: return percent(v);
                    case 25: return v > 0.5f ? "keep" : "from the patch";
                    case 32: case 35: return tuningBankText(v);
                    case 33: case 36: return tuningScaleText(a.p, j - 1, v);
                    case 45: return ptText(pt::kArpRate, float(double(v) * double(0.8f) + double(0.2f)));
                    default: break;
                }
                const int idx = partParamIndex(j);
                const tb::Param* tp = idx >= 0 ? tables().patch[size_t(idx)] : nullptr;
                return tp ? tbText(*tp, v) : percent(v);
            }
            case 'X': {
                if (a.i >= 0xac && a.i < 0xfc && (a.i - 0xac) % 5 == 0) return noteName(quant(v, 128.9f));
                const tb::Param* tp = tables().patch[size_t(a.i)];
                return tp ? tbText(*tp, v) : percent(v);
            }
            case 'E': {
                const int t = elemType(a.p, a.e);
                if (t == 5 && a.i == 0x22) return "wavetable " + std::to_string(quant(v, 128.9f) + 1);
                if (t == 0 && a.i >= 0x19 && a.i <= 0x24) {
                    const Knob* k = elemKnob(a.p, a.e, a.i - 0x19);
                    return k ? fxText(*k, v) : percent(v);
                }
                if (t == 0 && a.i == 0x13) {
                    const char* n = hs2::effectName(quant(v, 128.9f));
                    return n ? n : "none";
                }
                if (a.i == 0x01) return typeName(quant(v, 15.9f));
                if ((t == 2 || t == 3) && a.i == 0x12) {
                    const int n = quant(v, 128.9f);
                    const auto& names = t == 2 ? I.sampleBankNames : I.wavetableNames;
                    return names[size_t(std::clamp(n, 0, 128))].empty() ? "bank " + std::to_string(n) + " (none)"
                                                                         : names[size_t(n)];
                }
                if (t == 2 && a.i == 0x13)
                    return sampleWaveText(quant(E().getPartSetter(a.p, a.e, 0x12), 128.9f), quant(v, 128.9f));
                if (t == 3 && a.i == 0x13) return "wave " + std::to_string(quant(v, 128.9f) + 1);
                const tb::Param* tp = tables().elem(t, a.i);
                return tp ? tbText(*tp, v) : percent(v);
            }
        }
        return {};
    }

    bool readOnly(const Addr& a) {
        if (a.k == 'E') return a.i == 0x01 || (a.i == 0x13 && elemType(a.p, a.e) == 0);
        return false;
    }

    // ---- steps: delta texts away
    float idxValue(int n) { return (float(n) + 0.5f) / 128.9f; }

    void step(const Addr& a, int delta) {
        if (delta == 0 || readOnly(a)) return;
        const float v = value(a);
        const int dir = delta > 0 ? 1 : -1;
        auto res = [&](int r) { return std::clamp(float(std::lround(double(v) * r) + delta) / float(r), 0.f, 1.f); };
        switch (a.k) {
            case 'M': setValue(a, res(200)); return;
            case 'A': setValue(a, float(std::clamp(I.active + dir, 0, kParts - 1))); return;
            case 'C': setValue(a, float(std::clamp(I.channel[size_t(a.p)] + delta, 0, kAllChannels))); return;
            case 'B': {
                if (I.browse.empty()) return;
                const int cur = I.browseTarget[size_t(a.p)];
                setValue(a, float(cur < 0 ? (delta > 0 ? delta - 1 : int(I.browse.size()) + delta) : cur + delta));
                return;
            }
            case 'K': {
                if (I.banks.empty()) return;
                const int cur = int(v);
                setValue(a, float(cur < 0 ? 0 : std::clamp(cur + delta, 0, int(I.banks.size()) - 1)));
                return;
            }
            case 'F': setValue(a, float(I.gfxTarget[size_t(a.p)] + delta)); return;
            case 'Z':
                switch (a.i) {
                    case 0: case 1: case 2: case 9: setValue(a, dir > 0 ? 1.f : 0.f); return;
                    case 3: setValue(a, float(std::clamp(I.set.memory + dir, 0, 2))); return;
                    case 7: setValue(a, float(I.set.tuneSemis + delta)); return;
                    case 8: setValue(a, float(I.set.tuneCents + delta)); return;
                    default: setValue(a, res(100)); return;
                }
            case 'G': {
                if (a.i >= 1 && a.i <= 16) {
                    const Knob* k = gfxKnob(a.p, a.i - 1);
                    setValue(a, k ? stepFx(*k, v, delta) : res(100));
                } else if (const pt::Table* t = sendFxTable(a.i)) {
                    setValue(a, stepPt(*t, v, delta));
                }
                return;
            }
            case 'P': {
                const int j = a.i;
                if (j >= 56) {
                    const int s = (j - 56) / 24, jj = (j - 56) % 24;
                    if (jj >= 1 && jj <= 16) {
                        const Knob* k = psendKnob(a.p, s, jj - 1);
                        setValue(a, k ? stepFx(*k, v, delta) : res(100));
                    } else if (const pt::Table* t = sendFxTable(jj)) {
                        setValue(a, stepPt(*t, v, delta));
                    }
                    return;
                }
                switch (j) {
                    case 1: case 25: setValue(a, dir > 0 ? 1.f : 0.f); return;
                    case 2: case 5: case 6: case 7: case 8:
                    case 9: case 10: case 11: case 12: case 13: case 14: setValue(a, res(100)); return;
                    case 3: setValue(a, res(200)); return;
                    case 32: case 35: {  // none, then each tuning bank
                        std::vector<int> nums{-1};
                        if (I.lib)
                            for (const auto& t : I.lib->tunings) nums.push_back(t.number);
                        const int cur = quant(v, 128.9f);
                        int pos = 0;
                        for (size_t q = 0; q < nums.size(); ++q)
                            if (nums[q] == cur) pos = int(q);
                        pos = std::clamp(pos + delta, 0, int(nums.size()) - 1);
                        int n = nums[size_t(pos)];
                        if (n < 0) {  // a bank number without a file: no tuning
                            n = 127;
                            while (n > 0 && I.lib && I.lib->tuning(n) != nullptr) --n;
                        }
                        setValue(a, idxValue(n));
                        return;
                    }
                    case 33: case 36: {
                        const auto* t = I.lib ? I.lib->tuning(quant(E().getParameter(a.p * kPP + j - 1), 128.9f)) : nullptr;
                        const int count = t ? int(t->scales.size()) : 0;
                        if (count == 0) return;
                        setValue(a, idxValue(std::clamp(quant(v, 128.9f) + delta, 0, count - 1)));
                        return;
                    }
                    case 45: {
                        const float pv = float(double(v) * double(0.8f) + double(0.2f));
                        float np = stepPt(pt::kArpRate, pv, delta);
                        if (np < 0.2f) np = stepPt(pt::kArpRate, 0.2001f, 0);
                        setValue(a, std::clamp((np - 0.2f) / 0.8f, 0.f, 1.f));
                        return;
                    }
                    default: break;
                }
                const int idx = partParamIndex(j);
                const tb::Param* tp = idx >= 0 ? tables().patch[size_t(idx)] : nullptr;
                setValue(a, tp ? stepTable(*tp, v, delta) : res(100));
                return;
            }
            case 'X': {
                if (a.i >= 0xac && a.i < 0xfc && (a.i - 0xac) % 5 == 0) {  // arp step note: note number
                    setValue(a, idxValue(std::clamp(quant(v, 128.9f) + delta, 0, 127)));
                    return;
                }
                const tb::Param* tp = tables().patch[size_t(a.i)];
                setValue(a, tp ? stepTable(*tp, v, delta) : res(100));
                return;
            }
            case 'E': {
                const int t = elemType(a.p, a.e);
                if (t < 0) return;
                if (t == 5 && a.i == 0x22) {  // Analog wavetable: an entry of the core wavetable bank
                    setValue(a, idxValue(std::clamp(quant(v, 128.9f) + delta, 0, 127)));
                    return;
                }
                if (t == 0 && a.i >= 0x19 && a.i <= 0x24) {
                    const Knob* k = elemKnob(a.p, a.e, a.i - 0x19);
                    setValue(a, k ? stepFx(*k, v, delta) : res(100));
                    return;
                }
                if ((t == 2 || t == 3) && a.i == 0x12) {  // the banks that exist
                    const auto& names = t == 2 ? I.sampleBankNames : I.wavetableNames;
                    int n = quant(v, 128.9f);
                    for (int left = std::abs(delta); left > 0;) {
                        n += dir;
                        if (n < 0 || n > 128) return;
                        if (!names[size_t(n)].empty()) --left;
                    }
                    setValue(a, idxValue(n));
                    return;
                }
                if (a.i == 0x13 && (t == 2 || t == 3)) {
                    int count = 128;
                    if (t == 2) {
                        hs2::SampleBank* sb = I.content ? I.content->sampleBank(quant(E().getPartSetter(a.p, a.e, 0x12), 128.9f)) : nullptr;
                        count = sb ? int(sb->instruments.size()) : 0;
                    }
                    if (count == 0) return;
                    setValue(a, idxValue(std::clamp(quant(v, 128.9f) + delta, 0, std::min(count, 129) - 1)));
                    return;
                }
                const tb::Param* tp = tables().elem(t, a.i);
                setValue(a, tp ? stepTable(*tp, v, delta) : res(100));
                return;
            }
        }
    }

    // ---- layouts (JSON) for the editor pages: [{"t": title, "p": [[addr, name, readOnly], ...]}, ...]
    struct Page {
        std::string title;
        std::vector<int> idx;
    };
    static std::vector<int> range(int a, int b) {
        std::vector<int> v;
        for (int i = a; i <= b; ++i) v.push_back(i);
        return v;
    }
    std::string pagesJson(const std::vector<Page>& pages, const std::function<std::string(int)>& addr,
                          const std::function<std::string(int)>& nameOf, const std::function<bool(int)>& ro) {
        std::string out = "[";
        bool firstPage = true;
        for (const auto& pg : pages) {
            std::string items;
            for (int i : pg.idx) {
                const std::string n = nameOf(i);
                if (n.empty()) continue;
                if (!items.empty()) items += ",";
                items += "[" + jstr(addr(i)) + "," + jstr(n) + "," + (ro(i) ? "1" : "0") + "]";
            }
            if (items.empty()) continue;
            if (!firstPage) out += ",";
            firstPage = false;
            out += "{\"t\":" + jstr(pg.title) + ",\"p\":[" + items + "]}";
        }
        return out + "]";
    }

    std::string elemLayout(int p, int e) {
        const int t = elemType(p, e);
        if (t < 0) return "[]";
        std::vector<Page> pages;
        const std::vector<int> quick = {0x91, 0x04, 0x06, 0x07, 0x08, 0x09, 0x92, 0x05, 0x0a, 0x0b, 0x0c, 0x0d};
        if (t == 0) {
            std::vector<int> fx = {0x00, 0x13};
            for (int i = 0x19; i <= 0x24; ++i) fx.push_back(i);
            for (int i = 0x14; i <= 0x18; ++i) fx.push_back(i);
            pages.push_back({"Effect", fx});
            pages.push_back({"Quick Knobs", quick});
        } else {
            pages.push_back({"Main", {0x00, 0x01, 0x02, 0x03, 0x8a, 0x93, 0x95, 0x90, 0x94}});
            pages.push_back({typeName(t), range(0x11, 0x30)});
            pages.push_back({"Key and Velocity", range(0x31, 0x38)});
            pages.push_back({"Voice", range(0x39, 0x45)});
            pages.push_back({"Pitch", range(0x46, 0x4b)});
            pages.push_back({"Glide", range(0x4c, 0x54)});
            pages.push_back({"LFO 3", range(0x55, 0x58)});
            pages.push_back({"Envelope 1", range(0x59, 0x61)});
            pages.push_back({"Waveform", range(0x62, 0x66)});
            pages.push_back({"Filter", range(0x67, 0x6d)});
            pages.push_back({"Filter Envelope", range(0x6e, 0x78)});
            auto amp = range(0x7e, 0x89);
            amp.insert(amp.end(), {0x7b, 0x7c, 0x7d});
            pages.push_back({"Amp Envelope", amp});
            pages.push_back({"Output", range(0x8b, 0x8f)});
            pages.push_back({"Quick Knobs", quick});
            for (int r = 0; r < 16; ++r) {  // "Mod 3: LFO 1 > Pitch"
                const int b = 0xac + 5 * r;
                const std::string src = text(Addr{'E', p, e, b}), dst = text(Addr{'E', p, e, b + 3});
                pages.push_back({"Mod " + std::to_string(r + 1) + ": " + (src.empty() ? std::string("off") : src + " > " + dst),
                                 range(b, b + 4)});
            }
        }
        const std::string pre = "E" + std::to_string(p) + "." + std::to_string(e) + ".";
        return pagesJson(pages, [&](int i) { return pre + std::to_string(i); },
                         [&](int i) { return elemName(p, e, i); },
                         [&](int i) { return i == 0x01 || (t == 0 && i == 0x13); });
    }

    std::string patchLayout(int p) {
        std::vector<Page> pages;
        pages.push_back({"Patch", {0x01, 0x02, 0x44, 0x45, 0x46}});
        pages.push_back({"Piano", {0x54, 0x48, 0x47, 0x4b}});
        pages.push_back({"Patch LFOs", range(0x55, 0x5c)});
        std::vector<int> knobs = range(0x70, 0x78);
        for (int i : {0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x7b, 0x7c, 0x7d}) knobs.push_back(i);
        pages.push_back({"Knob Ranges", knobs});
        pages.push_back({"GM Controls", range(0x7e, 0x87)});
        pages.push_back({"Scenes", {0x95, 0x96, 0x97}});
        pages.push_back({"Arp Phrase", {0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x21}});
        for (int s = 0; s < 16; ++s) pages.push_back({"Arp Step " + std::to_string(s + 1), range(0xac + 5 * s, 0xac + 5 * s + 4)});
        const std::string pre = "X" + std::to_string(p) + ".";
        return pagesJson(pages, [&](int i) { return pre + std::to_string(i); }, [&](int i) { return patchName(i); },
                         [](int) { return false; });
    }

    // The part and global menus: lay:<group>[.<part>] -> pages of addresses (same JSON as the element pages).
    std::string groupLayout(const std::string& g) {
        const auto dot = g.find('.');
        const std::string nm = g.substr(0, dot);
        const int p = dot == std::string::npos ? I.active : std::clamp(std::atoi(g.c_str() + dot + 1), 0, kParts - 1);
        const std::string ps = std::to_string(p);
        auto P = [&ps](int j) { return "P" + ps + "." + std::to_string(j); };
        auto Ps = [&P](std::initializer_list<int> js) {
            std::vector<std::string> v;
            for (int j : js) v.push_back(P(j));
            return v;
        };
        std::vector<std::pair<std::string, std::vector<std::string>>> pages;
        if (nm == "setup") {
            auto v = Ps({1, 2, 3, 5, 6, 7, 8, 24, 26, 25});
            v.insert(v.begin(), "C" + ps);
            pages.push_back({"Part Setup", v});
        } else if (nm == "knobs") {
            pages.push_back({"Hyperknobs", Ps({9, 10, 11, 12, 13, 14})});
        } else if (nm == "keys") {
            pages.push_back({"Keyboard", Ps({15, 16, 17, 18, 19, 20, 21, 22, 27, 28})});
        } else if (nm == "voice") {
            pages.push_back({"Voice and Glide", Ps({23, 29, 30, 31})});
        } else if (nm == "arp") {
            pages.push_back({"Arpeggiator", Ps({39, 40, 41, 42, 43, 44, 45, 46, 47})});
        } else if (nm == "tuning") {
            pages.push_back({"Microtuning", Ps({32, 33, 34, 35, 36, 37, 38})});
        } else if (nm == "sends") {
            for (int s = 0; s < 4; ++s) {
                const char* fx = hs2::effectName(partSendType(p, s));
                std::vector<std::string> v;
                for (int jj = 0; jj < 24; ++jj) v.push_back(P(56 + s * 24 + jj));
                pages.push_back({"Send FX " + std::to_string(s + 1) + ": " + (fx ? fx : "none"), v});
            }
        } else if (nm == "gfx") {
            for (int k = 0; k < 4; ++k) {
                std::vector<std::string> v{"F" + std::to_string(k)};
                for (int j = 0; j < 24; ++j) v.push_back("G" + std::to_string(k) + "." + std::to_string(j));
                const int type = E().globalFxType(k);
                const char* fx = type < 128 ? hs2::effectName(type) : nullptr;
                pages.push_back({"Effect " + std::to_string(k + 1) + ": " + (fx ? fx : "none"), v});
            }
        } else if (nm == "settings") {
            std::vector<std::string> v{"M"};
            for (int i = 0; i < 10; ++i) v.push_back("Z" + std::to_string(i));
            pages.push_back({"Settings", v});
        }
        std::string out = "[";
        bool firstPage = true;
        for (const auto& [title, addrs] : pages) {
            std::string items;
            for (const auto& s : addrs) {
                Addr a;
                if (!parseAddr(s.c_str(), a)) continue;
                const std::string n = name(a);
                if (n.empty()) continue;
                if (!items.empty()) items += ",";
                items += "[" + jstr(s) + "," + jstr(n) + "," + (readOnly(a) ? "1" : "0") + "]";
            }
            if (items.empty()) continue;
            if (!firstPage) out += ",";
            firstPage = false;
            out += "{\"t\":" + jstr(title) + ",\"p\":[" + items + "]}";
        }
        return out + "]";
    }

    std::string partInfo(int p) {
        const auto& pi = I.parts[size_t(p)];
        std::string k = "[";
        for (int i = 0; i < kKnobs; ++i) k += (i ? "," : "") + jstr(pi.knobNames[size_t(i)]);
        k += "]";
        bool loading = false;
        {
            std::lock_guard<std::mutex> lk(I.qmu);
            loading = I.loadGen[size_t(p)] != I.doneGen[size_t(p)].load();
        }
        loading = loading || I.restoring[size_t(p)].has_value();
        return "{\"ch\":" + std::to_string(I.channel[size_t(p)]) + ",\"empty\":" + (pi.empty ? "1" : "0") +
               ",\"name\":" + jstr(pi.empty ? std::string("empty") : pi.name) + ",\"bank\":" + jstr(pi.bankName) +
               ",\"bn\":" + std::to_string(pi.bank) + ",\"num\":" + std::to_string(pi.index + 1) +
               ",\"pos\":" + std::to_string(I.browseTarget[size_t(p)]) + ",\"loading\":" + (loading ? "1" : "0") +
               ",\"knobs\":" + k + ",\"warn\":" + jstr(pi.warning) + "}";
    }

    std::string elemsJson(int p) {
        std::string out = "[";
        bool first = true;
        for (const auto& el : I.parts[size_t(p)].elems) {
            if (!first) out += ",";
            first = false;
            std::string n = el.name;
            if (el.type == 0) {
                const char* fx = hs2::effectName(quant(E().getPartSetter(p, el.setter, 0x13), 128.9f));
                if (n.empty() && fx) n = fx;
            }
            out += "[" + std::to_string(el.setter) + "," + std::to_string(el.type) + "," + jstr(typeName(el.type)) + "," +
                   jstr(n.empty() ? "Element " + std::to_string(el.setter) : n) + "]";
        }
        return out + "]";
    }
};

// ------------------------------------------------------------------------------------------------ state

std::string Instance::stateJson() {
    std::string s = "{\"v\":2,\"active\":" + std::to_string(active);
    s += ",\"set\":[" + std::to_string(set.midiPc) + "," + std::to_string(set.ringOut) + "," +
         std::to_string(set.cutOnLoad) + "," + std::to_string(set.memory) + "," + fnum(set.hype) + "," +
         fnum(set.velSens) + "," + fnum(set.touchSens) + "," + std::to_string(set.tuneSemis) + "," +
         std::to_string(set.tuneCents) + "," + std::to_string(set.preload) + "]";
    if (masterTouch) s += ",\"master\":" + fnum(*masterTouch);
    s += ",\"parts\":[";
    for (int p = 0; p < kParts; ++p) {
        if (p) s += ",";
        s += "{\"ch\":" + std::to_string(channel[size_t(p)]);
        const auto& r = restoring[size_t(p)];
        int bank = r ? r->bank : parts[size_t(p)].bank, idx = r ? r->index : parts[size_t(p)].index;
        std::string name = r ? r->name : parts[size_t(p)].name;
        if (bank >= 0 && (r || !parts[size_t(p)].empty)) {
            s += ",\"bank\":" + std::to_string(bank) + ",\"idx\":" + std::to_string(idx) + ",\"name\":" + jstr(name);
            std::string ed;
            auto add = [&ed](int e, int i, float v) {
                if (!ed.empty()) ed += ",";
                ed += "[" + std::to_string(e) + "," + std::to_string(i) + "," + fnum(v) + "]";
            };
            if (r) {
                for (const auto& [key, v] : r->edits) add(key.first, key.second, v);
            } else {
                for (const auto& [j, v] : touch[size_t(p)]) add(-1, j, v);
                for (const auto& [key, v] : edits[size_t(p)]) add(key.first, key.second, v);
            }
            s += ",\"ed\":[" + ed + "]";
        }
        s += "}";
    }
    s += "],\"gfx\":[";
    for (int k = 0; k < 4; ++k) {
        if (k) s += ",";
        s += "{\"path\":" + jstr(gfxPath[size_t(k)]) + ",\"t\":[";
        bool first = true;
        for (const auto& [j, v] : gfxTouch[size_t(k)]) {
            if (!first) s += ",";
            first = false;
            s += "[" + std::to_string(j) + "," + fnum(v) + "]";
        }
        s += "]}";
    }
    return s + "]}";
}

void Instance::restoreState(const std::string& str) {
    JVal root;
    if (!parseJson(str, root) || root.t != JVal::Obj) {
        logf("state: could not parse (%zu bytes)", str.size());
        return;
    }
    std::array<std::optional<LoadJob>, kParts> loads;
    std::array<FxJob, 4> fx;
    std::array<bool, 4> fxAny{};
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!ready) {  // before the content scan: later
            pendingState = str;
            return;
        }
        everRestored = true;
        active = std::clamp(int(root.num("active", 0)), 0, kParts - 1);
        if (const JVal* sv = root.get("set"); sv && sv->t == JVal::Arr && sv->a.size() >= 10) {
            auto n = [&](size_t i) { return sv->a[i].t == JVal::Num ? sv->a[i].n : 0.0; };
            set.midiPc = n(0) > 0.5;
            set.ringOut = n(1) > 0.5;
            set.cutOnLoad = n(2) > 0.5;
            set.memory = std::clamp(int(n(3)), 0, 2);
            set.hype = float(n(4));
            set.velSens = float(n(5));
            set.touchSens = float(n(6));
            set.tuneSemis = std::clamp(int(n(7)), -12, 12);
            set.tuneCents = std::clamp(int(n(8)), -50, 50);
            if (root.num("v", 1) >= 2) set.preload = n(9) > 0.5;  // v1 saved the old default (off) unasked
            if (engine) {
                engine->setMemoryMode(memoryValue(set.memory));
                applySettings();
            }
        }
        if (const JVal* m = root.get("master"); m && m->t == JVal::Num && engine) {
            masterTouch = float(m->n);
            engine->setParameter(0, float(m->n));
        }
        const JVal* ps = root.get("parts");
        for (int p = 0; p < kParts; ++p) {
            const JVal* pv = ps && ps->t == JVal::Arr && size_t(p) < ps->a.size() ? &ps->a[size_t(p)] : nullptr;
            if (!pv || pv->t != JVal::Obj) continue;
            channel[size_t(p)] = std::clamp(int(pv->num("ch", p)), 0, kAllChannels);
            const int bank = int(pv->num("bank", -1));
            touch[size_t(p)].clear();
            edits[size_t(p)].clear();
            if (bank < 0) {
                if (engine && !parts[size_t(p)].empty) engine->clearPart(p);
                parts[size_t(p)] = PartInfo();
                browseTarget[size_t(p)] = -1;
                restoring[size_t(p)].reset();
                continue;
            }
            LoadJob j;
            j.bank = bank;
            j.index = int(pv->num("idx", 0));
            j.name = pv->str("name");
            j.restore = true;
            if (const JVal* ed = pv->get("ed"); ed && ed->t == JVal::Arr)
                for (const auto& row : ed->a)
                    if (row.t == JVal::Arr && row.a.size() == 3)
                        j.edits[{int(row.a[0].n), int(row.a[1].n)}] = float(row.a[2].n);
            for (size_t i = 0; i < browse.size(); ++i)
                if (browse[i].bank == j.bank && browse[i].index == j.index) browseTarget[size_t(p)] = int(i);
            restoring[size_t(p)] = j;
            loads[size_t(p)] = std::move(j);
        }
        const JVal* gs = root.get("gfx");
        for (int k = 0; k < 4; ++k) {
            const JVal* gv = gs && gs->t == JVal::Arr && size_t(k) < gs->a.size() ? &gs->a[size_t(k)] : nullptr;
            if (!gv || gv->t != JVal::Obj) continue;
            FxJob j;
            j.preset = -2;
            j.restore = true;
            j.path = gv->str("path");
            if (j.path == "none") {
                j.preset = -1;
                j.path.clear();
            }
            if (const JVal* t = gv->get("t"); t && t->t == JVal::Arr)
                for (const auto& row : t->a)
                    if (row.t == JVal::Arr && row.a.size() == 2) j.touch[int(row.a[0].n)] = float(row.a[1].n);
            fxAny[size_t(k)] = !j.path.empty() || j.preset == -1 || !j.touch.empty();
            gfxTarget[size_t(k)] = -1;
            if (j.preset == -2 && !j.path.empty() && lib)
                for (size_t i = 0; i < lib->effects.size(); ++i)
                    if (lib->effects[i].rel.generic_string() == j.path) gfxTarget[size_t(k)] = int(i);
            fx[size_t(k)] = std::move(j);
        }
    }
    for (int p = 0; p < kParts; ++p)
        if (loads[size_t(p)]) post(p, std::move(*loads[size_t(p)]));
    for (int k = 0; k < 4; ++k)
        if (fxAny[size_t(k)]) postFx(k, std::move(fx[size_t(k)]));
}

// ------------------------------------------------------------------------------------------------ plugin API

int writeOut(char* buf, int len, const std::string& s) {
    if (!buf || len <= 0) return -1;
    const int n = std::min(int(s.size()), len - 1);
    std::memcpy(buf, s.data(), size_t(n));
    buf[n] = 0;
    return n;
}

std::string fmtFloat(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.4f", double(v));
    return b;
}

void* v2_create(const char* moduleDir, const char* /*defaults*/) {
    auto* I = new Instance();
    I->moduleDir = moduleDir ? moduleDir : ".";
    I->contentRoot = fs::path(I->moduleDir) / "content";
    if (const char* env = std::getenv("HS2_CONTENT"); env && *env) I->contentRoot = env;  // desktop tests
    if (const char* env = std::getenv("HS2_PRELOAD_MB"); env && *env)  // desktop tests
        I->preloadBudget = size_t(std::max(0.0, std::atof(env)) * 1048576.0);
    I->buf.resize(2 * 4096);
    I->events.reserve(512);
    I->worker = std::thread([I] { I->workerLoop(); });
    logf("instance created, content folder %s", I->contentRoot.c_str());
    return I;
}

void v2_destroy(void* inst) {
    auto* I = static_cast<Instance*>(inst);
    if (!I) return;
    {
        std::lock_guard<std::mutex> lk(I->qmu);
        I->quit = true;
    }
    I->cv.notify_all();
    if (I->worker.joinable()) I->worker.join();
    delete I;
}

void programChange(Instance& I, int p, int prog) {
    if (!I.lib) return;
    const int bank = I.sawLsb[size_t(p)] ? I.bankMsb[size_t(p)] * 128 + I.bankLsb[size_t(p)] : I.bankMsb[size_t(p)];
    const int li = I.lib->find(bank, prog);
    if (li < 0) return;
    const auto& ref = I.lib->patches[size_t(li)];  // a reference, no copy: this runs on the audio thread
    if ((ref.copyProtected && !ref.captured) || ref.combi) return;
    for (size_t i = 0; i < I.browse.size(); ++i)
        if (I.browse[i].bank == bank && I.browse[i].index == prog) I.browseTarget[size_t(p)] = int(i);
    LoadJob j;
    j.bank = bank;
    j.index = prog;
    I.post(p, std::move(j));
}

void v2_on_midi(void* inst, const uint8_t* msg, int len, int /*source*/) {
    auto* I = static_cast<Instance*>(inst);
    if (!I || !msg || len < 1) return;
    const uint8_t st = msg[0];
    if (st < 0x80 || st >= 0xf0) return;  // the transport comes from the host clock API, not MIDI realtime bytes
    const int ch = st & 0x0f, type = st & 0xf0;
    const uint8_t d1 = len > 1 ? msg[1] & 0x7f : 0, d2 = len > 2 ? msg[2] & 0x7f : 0;
    std::lock_guard<std::mutex> lk(I->mu);
    std::string routed;
    for (int p = 0; p < kParts; ++p) {
        const int c = I->channel[size_t(p)];
        if (c != kAllChannels && c != ch) continue;
        routed += " part" + std::to_string(p + 1) + (I->parts[size_t(p)].empty ? "(empty)" : "");
        if (type == 0xb0 && (d1 == 0 || d1 == 32)) {  // bank select (the part keeps it for its next program change)
            if (d1 == 0) I->bankMsb[size_t(p)] = d2;
            else I->bankLsb[size_t(p)] = d2, I->sawLsb[size_t(p)] = true;
            continue;
        }
        if (type == 0xc0) {
            if (I->set.midiPc) programChange(*I, p, d1);
            continue;
        }
        if (!I->engine || I->events.size() >= 512) continue;
        hs2::MidiEvent e;
        e.offset = 0;
        e.data[0] = uint8_t(type | p);  // engine part p listens on channel p
        e.data[1] = d1;
        e.data[2] = d2;
        I->events.push_back(e);
    }
    if (I->midiLogLeft > 0) {
        --I->midiLogLeft;
        logf("midi in %02x %02x %02x (channel %d) ->%s%s", st, d1, d2, ch + 1, routed.empty() ? " no part" : routed.c_str(),
             I->engine ? "" : " (engine not ready)");
    }
    if (type == 0x90 && d2 > 0 && !routed.empty()) I->levelLogLeft = 12;
}

void v2_render(void* inst, int16_t* out, int frames) {
    auto* I = static_cast<Instance*>(inst);
    if (!out || frames <= 0) return;
    std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
    if (!I) return;
    std::lock_guard<std::mutex> lk(I->mu);
    if (!I->engine) {
        I->events.clear();
        return;
    }
    if (!I->cmds.empty()) I->applyCommands();
    // Move's clock as the VST transport: tempo, play state and the song position in quarter notes.
    hs2::HostTransport tr;
    double bpm = g_host && g_host->get_bpm ? double(g_host->get_bpm()) : 120.0;
    if (!(bpm >= 20.0 && bpm <= 999.0)) bpm = 120.0;
    const int clock = g_host && g_host->get_clock_status ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_UNAVAILABLE;
    const double beat = g_host && g_host->get_beat_position ? g_host->get_beat_position() : -1.0;
    tr.bpm = bpm;
    tr.tempoValid = true;
    tr.playing = beat >= 0.0 || clock == MOVE_CLOCK_STATUS_RUNNING;
    tr.ppqValid = beat >= 0.0;
    tr.ppq = beat >= 0.0 ? beat : 0.0;
    tr.barValid = beat >= 0.0;
    tr.barStart = beat >= 0.0 ? std::floor(beat / 4.0) * 4.0 : 0.0;
    tr.sigNum = 4;
    tr.sigDen = 4;
    tr.sigValid = true;
    for (int start = 0; start < frames;) {
        const int n = std::min(frames - start, int(I->buf.size() / 2));
        std::fill(I->buf.begin(), I->buf.begin() + 2 * n, 0.f);
        I->engine->processBlock(I->buf.data(), n, start == 0 ? I->events.data() : nullptr,
                                start == 0 ? int(I->events.size()) : 0, start == 0 ? &tr : nullptr);
        for (int i = 0; i < 2 * n; ++i) {
            const float x = std::clamp(I->buf[size_t(i)], -1.f, 1.f);
            out[2 * start + i] = int16_t(std::lrint(x * 32767.f));
        }
        start += n;
    }
    I->events.clear();
    if (I->levelLogLeft > 0) {
        for (int i = 0; i < 2 * frames; ++i) I->levelPeak = std::max(I->levelPeak, float(std::abs(int(out[i]))) / 32768.f);
        if (++I->levelBlocks >= 100) {  // about 0.3 s
            logf("output peak %.4f (master %.2f)", double(I->levelPeak), double(I->engine->getParameter(0)));
            I->levelPeak = 0;
            I->levelBlocks = 0;
            --I->levelLogLeft;
        }
    }
}

void v2_set_param(void* inst, const char* key, const char* val) {
    auto* I = static_cast<Instance*>(inst);
    if (!I || !key) return;
    const std::string k = key, v = val ? val : "";
    if (k == "state") {
        I->restoreState(v);
        return;
    }
    if (k == "rescan") {
        {
            std::lock_guard<std::mutex> lk(I->qmu);
            I->scanRequested = true;
        }
        I->cv.notify_one();
        return;
    }
    std::lock_guard<std::mutex> lk(I->mu);
    if (!I->engine) {
        if (k == "part") I->active = std::clamp(std::atoi(v.c_str()), 0, kParts - 1);
        return;
    }
    Api api{*I};
    Addr a;
    if (k.rfind("v:", 0) == 0) {
        if (parseAddr(k.c_str() + 2, a) && !api.readOnly(a)) api.setValue(a, std::strtof(v.c_str(), nullptr));
    } else if (k.rfind("step:", 0) == 0) {
        if (parseAddr(k.c_str() + 5, a)) api.step(a, std::atoi(v.c_str()));
    } else if (k == "part") {
        I->active = std::clamp(std::atoi(v.c_str()), 0, kParts - 1);
    } else if (k.rfind("load:", 0) == 0) {  // load:<part> = browse index
        const int p = std::atoi(k.c_str() + 5);
        if (p >= 0 && p < kParts) api.loadBrowse(p, std::atoi(v.c_str()));
    } else if (k == "preset") {
        api.loadBrowse(I->active, std::atoi(v.c_str()));
    } else if (k.size() == 3 && k[0] == 'h' && k[1] == 'k' && k[2] >= '1' && k[2] <= '6') {  // chain knobs
        api.setValue(Addr{'P', I->active, 0, 9 + (k[2] - '1')}, std::clamp(std::strtof(v.c_str(), nullptr), 0.f, 1.f));
    } else if (k == "level") {
        api.setValue(Addr{'P', I->active, 0, 2}, std::clamp(std::strtof(v.c_str(), nullptr), 0.f, 1.f));
    } else if (k == "pan") {
        api.setValue(Addr{'P', I->active, 0, 3}, std::clamp(std::strtof(v.c_str(), nullptr), 0.f, 1.f));
    } else if (k == "panic" || k == "all_notes_off") {
        for (int p = 0; p < kParts; ++p)
            for (uint8_t cc : {uint8_t(64), uint8_t(123), uint8_t(120)}) {
                hs2::MidiEvent e;
                e.data[0] = uint8_t(0xb0 | p);
                e.data[1] = cc;
                e.data[2] = 0;
                if (I->events.size() < 512) I->events.push_back(e);
            }
    }
}

int v2_get_param(void* inst, const char* key, char* buf, int len) {
    auto* I = static_cast<Instance*>(inst);
    if (!I || !key) return -1;
    const std::string k = key;
    if (k == "status") return writeOut(buf, len, I->getStatus());
    std::lock_guard<std::mutex> lk(I->mu);
    if (k == "ready") return writeOut(buf, len, std::to_string(I->scanState));
    if (k == "mem") return writeOut(buf, len, std::to_string(I->content ? I->content->decodedSampleBytes() : 0));
    if (k == "part") return writeOut(buf, len, std::to_string(I->active));
    if (k == "state") return writeOut(buf, len, I->stateJson());
    if (k == "preset_count") return writeOut(buf, len, std::to_string(I->browse.size()));
    if (k == "banks") {
        std::string s = "[";
        for (size_t i = 0; i < I->banks.size(); ++i) {
            const auto& b = I->banks[i];
            s += (i ? "," : "") + std::string("[") + std::to_string(b.number) + "," + jstr(b.name) + "," +
                 std::to_string(b.first) + "," + std::to_string(b.count) + "]";
        }
        return writeOut(buf, len, s + "]");
    }
    if (k.rfind("bankp:", 0) == 0) {  // the patch names of bank row i
        const int r = std::atoi(k.c_str() + 6);
        if (r < 0 || r >= int(I->banks.size())) return writeOut(buf, len, "[]");
        const auto& b = I->banks[size_t(r)];
        std::string s = "[";
        for (int i = 0; i < b.count; ++i) s += (i ? "," : "") + jstr(I->browse[size_t(b.first + i)].name);
        return writeOut(buf, len, s + "]");
    }
    if (k == "fx_count") return writeOut(buf, len, std::to_string(I->fxLabels.size()));
    if (!I->engine) return k.rfind("pinfo:", 0) == 0 ? writeOut(buf, len, "{\"empty\":1,\"name\":\"empty\",\"loading\":1}") : -1;
    Api api{*I};
    if (k == "preset") return writeOut(buf, len, std::to_string(I->browseTarget[size_t(I->active)]));
    if (k == "preset_name") return writeOut(buf, len, api.text(Addr{'B', I->active, 0, 0}));
    if (k.size() == 3 && k[0] == 'h' && k[1] == 'k' && k[2] >= '1' && k[2] <= '6')
        return writeOut(buf, len, fmtFloat(api.value(Addr{'P', I->active, 0, 9 + (k[2] - '1')})));
    if (k == "level") return writeOut(buf, len, fmtFloat(api.value(Addr{'P', I->active, 0, 2})));
    if (k == "pan") return writeOut(buf, len, fmtFloat(api.value(Addr{'P', I->active, 0, 3})));
    if (k.rfind("pinfo:", 0) == 0) {
        const int p = std::atoi(k.c_str() + 6);
        return p >= 0 && p < kParts ? writeOut(buf, len, api.partInfo(p)) : -1;
    }
    if (k.rfind("elems:", 0) == 0) {
        const int p = std::atoi(k.c_str() + 6);
        return p >= 0 && p < kParts ? writeOut(buf, len, api.elemsJson(p)) : -1;
    }
    if (k.rfind("elay:", 0) == 0) {  // elay:<part>.<element>
        Addr a;
        const std::string spec = "E" + k.substr(5) + ".0";
        if (!parseAddr(spec.c_str(), a)) return -1;
        return writeOut(buf, len, api.elemLayout(a.p, a.e));
    }
    if (k.rfind("lay:", 0) == 0) return writeOut(buf, len, api.groupLayout(k.substr(4)));
    if (k.rfind("play:", 0) == 0) {
        const int p = std::atoi(k.c_str() + 5);
        return p >= 0 && p < kParts ? writeOut(buf, len, api.patchLayout(p)) : -1;
    }
    Addr a;
    if (k.size() > 2 && k[1] == ':' && parseAddr(k.c_str() + 2, a)) {
        switch (k[0]) {
            case 'v': return writeOut(buf, len, fnum(api.value(a)));
            case 't': return writeOut(buf, len, api.text(a));
            case 'n': return writeOut(buf, len, api.name(a));
            case 'r': return writeOut(buf, len, api.readOnly(a) ? "1" : "0");
            default: return -1;
        }
    }
    return -1;
}

int v2_get_error(void* inst, char* buf, int len) {
    auto* I = static_cast<Instance*>(inst);
    if (!I) return 0;
    std::lock_guard<std::mutex> lk(I->mu);
    if (I->scanState != -1) return 0;
    return writeOut(buf, len, "Hypersonic 2 content not found in " + I->contentRoot.string());
}

plugin_api_v2_t g_api = {MOVE_PLUGIN_API_VERSION_2, v2_create, v2_destroy, v2_on_midi, v2_set_param,
                         v2_get_param, v2_get_error, v2_render};

}  // namespace

extern "C" __attribute__((visibility("default"))) plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host) {
    g_host = host;
    return &g_api;
}
