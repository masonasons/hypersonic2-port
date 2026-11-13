#include "PresetExport.h"

#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif JUCE_MAC
#include <stdio.h>
#endif

#include <juce_audio_formats/juce_audio_formats.h>

#include "PluginProcessor.h"  // exposedIndex, PartParam (the host parameter order)
#include "hs2/bank.h"
#include "hs2/combi.h"
#include "hs2/content.h"
#include "hs2/engine.h"
#include "hs2/patch.h"

namespace hs2port {

namespace {

// The plugin's identity (plugin/CMakeLists.txt: PLUGIN_MANUFACTURER_CODE Hs2p, PLUGIN_CODE Hs2P). The VST3 component
// class ID is JUCE's (VST3Interface::jucePluginId): 0xABCDEF01 0x9182FAEB 'Hs2p' 'Hs2P', as in moduleinfo.json.
constexpr const char* kVst3ClassId = "ABCDEF019182FAEB4873327048733250";
constexpr juce::int64 kVst3Uid[4] = {0xABCDEF01, 0x9182FAEB, 0x48733270, 0x48733250};
constexpr juce::int64 kAuType = 0x61756d75;          // 'aumu'
constexpr juce::int64 kAuSubtype = 0x48733250;       // 'Hs2P'
constexpr juce::int64 kAuManufacturer = 0x48733270;  // 'Hs2p'
constexpr const char* kVendor = "HS2 Port";
constexpr const char* kPluginName = "HS2 Port";
constexpr const char* kReceipt = ".hs2port-presets.json";
constexpr int kReceiptVersion = 1;
constexpr const char* kStagePrefix = ".hs2port-install-";
constexpr const char* kCommitMarker = "replacement-started";

auto nativePath(const juce::File& f) {
#if JUCE_WINDOWS
    return std::filesystem::path(f.getFullPathName().toWideCharPointer());
#else
    return std::filesystem::path(f.getFullPathName().toRawUTF8());
#endif
}

bool occupied(const juce::File& file) { return file.exists() || file.isSymbolicLink(); }

bool moveWithoutReplacing(const juce::File& from, const juce::File& to) {
#if JUCE_WINDOWS
    return MoveFileExW(from.getFullPathName().toWideCharPointer(), to.getFullPathName().toWideCharPointer(), 0) != 0;
#elif JUCE_MAC
    return renamex_np(from.getFullPathName().toRawUTF8(), to.getFullPathName().toRawUTF8(), RENAME_EXCL) == 0;
#else
    if (occupied(to)) return false;
    std::error_code ec;
    std::filesystem::rename(nativePath(from), nativePath(to), ec);
    return !ec;
#endif
}

juce::String latin1(const std::string& s) {
    juce::String out;
    out.preallocateBytes(s.size() * 2);
    for (unsigned char c : s) out += juce::juce_wchar(c);
    return out;
}

juce::String bankTitle(const PatchRef& ref, const BankInfo* bank) {
    // Keep three-digit banks after the two-digit run even in a text-only sort.
    const auto number = (ref.bank >= 100 ? juce::String("Bank ") : juce::String{})
                      + juce::String(ref.bank).paddedLeft('0', 2);
    if (bank == nullptr) return number;
    const auto name = latin1(bank->name);
    const auto prefix = juce::String(ref.bank).paddedLeft('0', 3) + " ";
    return number + " " + (name.startsWith(prefix) ? name.substring(prefix.length()) : name);
}

juce::String safeFileName(const juce::String& name, const juce::String& fallback) {
    auto s = name.removeCharacters("\\/:*?\"<>|").replaceCharacter('\t', ' ').trim();
    while (s.endsWithChar('.')) s = s.dropLastCharacters(1).trimEnd();
    if (s.isEmpty()) s = fallback;
    return s.substring(0, 120);
}

// ---- MessagePack (the subset NKS uses), big-endian as the format requires
void mpWrite(juce::MemoryOutputStream& o, const juce::var& v);
void mpLen(juce::MemoryOutputStream& o, size_t n, int fix, int fixMax, int b16, int b32) {
    if (n <= size_t(fixMax)) o.writeByte(char(fix | int(n)));
    else if (n <= 0xffff) { o.writeByte(char(b16)); o.writeShortBigEndian(short(n)); }
    else { o.writeByte(char(b32)); o.writeIntBigEndian(int(n)); }
}
void mpString(juce::MemoryOutputStream& o, const juce::String& s) {
    const auto utf8 = s.toUTF8();
    const size_t n = s.getNumBytesAsUTF8();
    if (n < 32) o.writeByte(char(0xa0 | int(n)));
    else if (n < 256) { o.writeByte(char(0xd9)); o.writeByte(char(n)); }
    else if (n < 65536) { o.writeByte(char(0xda)); o.writeShortBigEndian(short(n)); }
    else { o.writeByte(char(0xdb)); o.writeIntBigEndian(int(n)); }
    o.write(utf8.getAddress(), n);
}
void mpInt(juce::MemoryOutputStream& o, juce::int64 x) {
    if (x >= 0 && x <= 127) o.writeByte(char(x));
    else if (x < 0 && x >= -32) o.writeByte(char(0xe0 | int(x + 32)));
    else if (x >= 0 && x <= 0xff) { o.writeByte(char(0xcc)); o.writeByte(char(x)); }
    else if (x >= 0 && x <= 0xffff) { o.writeByte(char(0xcd)); o.writeShortBigEndian(short(x)); }
    else if (x >= 0 && x <= 0xffffffffLL) { o.writeByte(char(0xce)); o.writeIntBigEndian(int(juce::uint32(x))); }
    else if (x >= -0x80000000LL && x <= 0x7fffffffLL) { o.writeByte(char(0xd2)); o.writeIntBigEndian(int(x)); }
    else { o.writeByte(char(0xd3)); o.writeInt64BigEndian(x); }
}
void mpWrite(juce::MemoryOutputStream& o, const juce::var& v) {
    if (v.isVoid() || v.isUndefined()) o.writeByte(char(0xc0));
    else if (v.isBool()) o.writeByte(char(bool(v) ? 0xc3 : 0xc2));
    else if (v.isInt() || v.isInt64()) mpInt(o, juce::int64(v));
    else if (v.isDouble()) { o.writeByte(char(0xcb)); o.writeDoubleBigEndian(double(v)); }
    else if (v.isString()) mpString(o, v.toString());
    else if (const auto* a = v.getArray()) {
        mpLen(o, size_t(a->size()), 0x90, 15, 0xdc, 0xdd);
        for (const auto& e : *a) mpWrite(o, e);
    } else if (const auto* obj = v.getDynamicObject()) {
        const auto& props = obj->getProperties();
        mpLen(o, size_t(props.size()), 0x80, 15, 0xde, 0xdf);
        for (const auto& p : props) {
            mpString(o, p.name.toString());
            mpWrite(o, p.value);
        }
    } else {
        o.writeByte(char(0xc0));
    }
}

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> items) {
    auto* d = new juce::DynamicObject();
    for (const auto& [k, v] : items) d->setProperty(k, v);
    return juce::var(d);
}

juce::var arr(std::initializer_list<juce::var> items) {
    juce::Array<juce::var> a;
    for (const auto& v : items) a.add(v);
    return juce::var(a);
}

// RIFF chunk: id, little-endian size, data, pad byte to an even size.
void riffChunk(juce::MemoryOutputStream& o, const char* id, const juce::MemoryBlock& data) {
    o.write(id, 4);
    o.writeInt(int(data.getSize()));
    o.write(data.getData(), data.getSize());
    if (data.getSize() & 1) o.writeByte(0);
}

// An NKS chunk body: version 1 (little-endian) and a MessagePack value.
juce::MemoryBlock nksMsgpack(const juce::var& v) {
    juce::MemoryOutputStream o;
    o.writeInt(1);
    mpWrite(o, v);
    return o.getMemoryBlock();
}

// A deterministic UUID per preset (Komplete Kontrol keeps favourites and tags by it across re-exports).
juce::String presetUuid(const PatchRef& ref) {
    const auto key = juce::String("hs2port/") + (ref.combi ? "combi/" : "patch/") + juce::String(ref.bank) + "/" +
                     juce::String(ref.index) + "/" + latin1(ref.name);
    juce::uint8 raw[16];
    juce::uint64 h1 = 1469598103934665603ULL, h2 = 0x9e3779b97f4a7c15ULL;
    for (auto c : key) {  // two FNV-1a style 64-bit hashes
        h1 = (h1 ^ juce::uint64(c)) * 1099511628211ULL;
        h2 = (h2 ^ juce::uint64(c)) * 0x100000001b3ULL + 0x632be59bd9b4e019ULL;
    }
    for (int i = 0; i < 8; ++i) {
        raw[i] = juce::uint8(h1 >> (8 * i));
        raw[8 + i] = juce::uint8(h2 >> (8 * i));
    }
    raw[6] = juce::uint8((raw[6] & 0x0f) | 0x50);  // RFC 4122 layout: version 5 (name-based), variant 10
    raw[8] = juce::uint8((raw[8] & 0x3f) | 0x80);
    return juce::Uuid(raw).toDashedString();
}

// NKS sound type per factory bank (Komplete Kontrol's browser filters by these).
juce::var nksTypes(const PatchRef& ref) {
    struct T { int bank; const char* type; const char* sub; };
    static constexpr T kTypes[] = {
        {1, "Drums", "Kit"}, {2, "Percussion", ""}, {3, "Drums", "Kit"}, {4, "Percussion", ""}, {5, "Drums", "Kit"},
        {6, "Drums", "Loop"}, {7, "Piano / Keys", "Grand Piano"}, {8, "Piano / Keys", "Electric Piano"},
        {9, "Piano / Keys", "Clavinet"}, {10, "Organ", ""}, {11, "Piano / Keys", "Harpsichord"},
        {12, "Mallet Instruments", ""}, {13, "Mallet Instruments", "Bell"}, {14, "Mallet Instruments", ""},
        {15, "Bass", "Synth Bass"}, {16, "Bass", "Upright Bass"}, {17, "Bass", "Electric Bass"},
        {18, "Guitar", "Acoustic Guitar"}, {19, "Guitar", "Electric Guitar"}, {20, "Guitar", "Electric Guitar"},
        {21, "Bowed Strings", "Ensemble"}, {22, "Bowed Strings", ""}, {23, "Sound Effects", ""}, {24, "Vocal", ""},
        {25, "Brass", "Section"}, {26, "Brass", ""}, {27, "Reed Instruments", "Saxophone"},
        {28, "Reed Instruments", ""}, {30, "Reed Instruments", ""}, {31, "Synth Pad", ""}, {32, "Synth Pad", ""},
        {33, "Synth Pad", ""}, {34, "Soundscapes", ""}, {35, "Synth Misc", ""}, {36, "Synth Misc", ""},
        {37, "Synth Misc", ""}, {38, "Synth Misc", ""}, {39, "Synth Lead", ""}, {40, "Synth Lead", ""},
        {41, "Sound Effects", ""}, {42, "Sound Effects", ""}, {79, "Drums", "Kit"}, {80, "Synth Misc", ""},
        {81, "Synth Misc", ""}, {82, "Piano / Keys", "Grand Piano"}, {83, "Organ", ""}};
    if (!ref.combi)
        for (const auto& t : kTypes)
            if (t.bank == ref.bank) return arr({arr({juce::String(t.type), juce::String(t.sub)})});
    return juce::var(juce::Array<juce::var>());
}

juce::var knob(int id, const juce::String& name, const juce::String& section = {}) {
    auto v = obj({{"autoname", false}, {"id", id}, {"name", name}, {"vflag", false}});
    if (section.isNotEmpty()) v.getDynamicObject()->setProperty("section", section);
    return v;
}

// Komplete Kontrol's knob pages (eight knobs each). Ids are host parameter ids = indices (legacy parameter ids).
juce::var nksKnobPages(const PatchRef& ref, const hs2::Patch* patch, const hs2::Combi* combi) {
    juce::Array<juce::var> pages, page;
    auto finishPage = [&] {
        if (!page.isEmpty()) pages.add(juce::var(page));
        page.clear();
    };
    auto add = [&](int id, const juce::String& name, const juce::String& group) {
        if (page.size() == 8) finishPage();
        page.add(knob(id, name, page.isEmpty() ? group : juce::String()));
    };
    auto mixer = [&](int part, const juce::String& group) {
        finishPage();
        add(exposedIndex(part, ppLevel), "Level", group);
        add(exposedIndex(part, ppPan), "Pan", group);
        add(exposedIndex(part, ppMute), "Mute", group);
        for (int s = 0; s < kNumSends; ++s)
            add(exposedIndex(part, ppSend1 + s), "Send " + juce::String(s + 1), group);
        if (!ref.combi) add(0, "Master Volume", group);
    };
    auto hyperknobs = [&](int part, const hs2::Patch* sound, const juce::String& group) {
        finishPage();
        if (sound == nullptr) return;
        for (int k = 0; k < kNumKnobs; ++k) {
            const auto name = latin1(sound->quickControls[size_t(k)]).trim();
            if (name.isNotEmpty()) add(exposedIndex(part, ppKnob1 + k), name, group);
        }
    };
    if (ref.combi) {
        add(0, "Master Volume", "Combi");
        finishPage();
    } else {
        hyperknobs(0, patch, "Hyperknobs");
        mixer(0, "Mixer");
    }
    const int partCount = ref.combi ? kNumParts : 1;
    for (int part = 0; part < partCount; ++part) {
        const auto prefix = ref.combi ? "P" + juce::String(part + 1) + " " : juce::String();
        if (ref.combi) {
            if (combi == nullptr || !combi->parts[size_t(part)]) continue;
            hyperknobs(part, &*combi->parts[size_t(part)], prefix + "Hyperknobs");
            mixer(part, prefix + "Mixer");
        }
        auto group = [&](const char* title, std::initializer_list<int> engineIndices) {
            finishPage();
            for (int j : engineIndices)
                for (int e = 0; e < kExtPerPart; ++e)
                    if (kExtDefs[e].j == j) add(extIndex(part, e), kExtDefs[e].name, prefix + title);
        };
        group("Voice", {4, 21, 22, 23, 29, 30, 31, 24});
        group("Key and Velocity", {15, 16, 17, 18, 19, 20, 27, 28});
        group("Tuning", {32, 33, 34, 35, 36, 37, 38});
        group("Arpeggiator", {39, 40, 41, 42, 43, 44, 45, 46});
        group("Arp and Part", {47});
        add(partExtraIndex(part, pxTrim), "Trim", prefix + "Arp and Part");
        add(partExtraIndex(part, pxKeep), "Keep", prefix + "Arp and Part");
        if (part > 0) add(linkIndex(part), "Link", prefix + "Arp and Part");
    }
    for (int slot = 0; slot < kGfxSlots; ++slot) {
        finishPage();
        const auto title = "Global FX " + juce::String(slot + 1);
        add(gfxIndex(slot, 0), "Mute", title);
        for (int j = 17; j < kGfxParams; ++j) add(gfxIndex(slot, j), gfxDef(j).name, title);
    }
    finishPage();
    return obj({{"ni8", juce::var(pages)}});
}

juce::MemoryBlock makeNksPreset(const PatchRef& ref, const hs2::Patch* patch, const hs2::Combi* combi, const BankInfo* bank,
                                const juce::MemoryBlock& vstPreset, int patchNumberWidth) {
    const auto name = juce::String(ref.index + 1).paddedLeft('0', patchNumberWidth) + " " + latin1(ref.name);
    const auto nisi = obj({{"author", patch != nullptr ? latin1(patch->author).trim() : juce::String()},
                           {"bankchain", arr({juce::String(kPluginName), bankTitle(ref, bank), juce::String()})},
                           {"characters", juce::var(juce::Array<juce::var>())},
                           {"comment", patch != nullptr ? latin1(patch->description).trim() : juce::String("Combi")},
                           {"deviceType", "INST"},
                           {"name", name},
                           {"types", nksTypes(ref)},
                           {"uuid", presetUuid(ref)},
                           {"vendor", kVendor}});
    const auto plid = obj({{"VST.magic", 0},
                           {"VST3.uid", arr({kVst3Uid[0], kVst3Uid[1], kVst3Uid[2], kVst3Uid[3]})},
                           {"pluginName", kPluginName},
                           {"pluginVendor", kVendor}});
    juce::MemoryOutputStream pchk;
    pchk.writeInt(1);
    pchk.write(vstPreset.getData(), vstPreset.getSize());

    juce::MemoryOutputStream body;
    body.write("NIKS", 4);
    riffChunk(body, "NISI", nksMsgpack(nisi));
    riffChunk(body, "NICA", nksMsgpack(nksKnobPages(ref, patch, combi)));
    riffChunk(body, "PLID", nksMsgpack(plid));
    riffChunk(body, "PCHK", pchk.getMemoryBlock());
    juce::MemoryOutputStream o;
    o.write("RIFF", 4);
    o.writeInt(int(body.getDataSize()));
    o.write(body.getData(), body.getDataSize());
    return o.getMemoryBlock();
}

// ---- Komplete Kontrol previews

constexpr double kPreviewRate = 44100.0;
constexpr double kPreviewSeconds = 4.0;   // phrase (at most 2.75 s) and its tail
constexpr double kPreviewFade = 0.4;      // fade-out at the end
constexpr float kPreviewPeak = 0.7f;      // normalised to -3 dBFS ...
constexpr float kPreviewMaxGain = 4.0f;   // ... with at most +12 dB

struct PreviewNote { double start, length; int pitch, velocity; };

// The phrase for a preset, chosen by its NKS sound type (nksTypes): 120 bpm eighth notes on MIDI channel 1.
std::vector<PreviewNote> previewPhrase(const PatchRef& ref) {
    juce::String type, sub;
    if (const auto* t = nksTypes(ref).getArray(); t != nullptr && !t->isEmpty())
        if (const auto* pair = (*t)[0].getArray(); pair != nullptr && pair->size() >= 2)
            type = (*pair)[0].toString(), sub = (*pair)[1].toString();
    std::vector<PreviewNote> n;
    const double e = 0.25;  // an eighth note at 120 bpm
    if (type == "Drums" && sub != "Loop") {  // a groove on the GM drum map (kick 36, snare 38, hats 42 / 46)
        const int kick[] = {1, 0, 0, 0, 1, 1, 0, 0}, snare[] = {0, 0, 1, 0, 0, 0, 1, 0};
        for (int s = 0; s < 8; ++s) {
            if (kick[s]) n.push_back({s * e, 0.2, 36, 110});
            if (snare[s]) n.push_back({s * e, 0.2, 38, 105});
            n.push_back({s * e, 0.2, s == 7 ? 46 : 42, s % 2 == 0 ? 95 : 75});
        }
        n.push_back({8 * e, 0.5, 36, 110});
        n.push_back({8 * e, 0.5, 49, 100});
    } else if (type == "Bass") {
        const int pitch[] = {36, 36, 43, 46, 48, 43};
        const double len[] = {0.45, 0.2, 0.2, 0.45, 0.2, 0.2};
        const double at[] = {0, 2, 3, 4, 6, 7};
        for (int i = 0; i < 6; ++i) n.push_back({at[i] * e, len[i], pitch[i], 100});
        n.push_back({8 * e, 0.75, 36, 100});
    } else if (type == "Percussion" || type == "Mallet Instruments") {
        const int pitch[] = {60, 62, 64, 65, 67, 65, 64, 62};
        for (int i = 0; i < 8; ++i) n.push_back({i * e, 0.2, pitch[i], i % 2 == 0 ? 105 : 85});
        n.push_back({8 * e, 0.6, 60, 105});
    } else if (sub == "Loop" || type == "Sound Effects") {  // loops and effects run on one held key
        n.push_back({0, 2.5, 60, 100});
    } else {  // keys, pads, leads, combis ...: a rising C major arpeggio, then the chord with the bass note
        const int arp[] = {60, 64, 67, 72};
        for (int i = 0; i < 4; ++i) n.push_back({i * e, 0.24, arp[i], 95});
        for (int p : {48, 60, 64, 67}) n.push_back({4 * e, 1.75, p, 95});
    }
    return n;
}

// Renders notes with the engine (stereo, kPreviewRate, `seconds` long) as the plugin plays the preset: part 1 holds
// the patch (or the combi is loaded), effect tails ring out, the host transport runs at 120 bpm. Not normalised.
juce::AudioBuffer<float> renderNotes(hs2::Content& content, const PatchRef& ref, const hs2::Patch* patch,
                                     const std::vector<PreviewNote>& notes, double seconds) {
    hs2::Engine eng(content, float(kPreviewRate));
    eng.setRingOut(true);
    if (ref.combi) {
        const auto path = hs2::combiPath(content.root(), ref.bank, ref.index);
        if (path.empty()) throw std::runtime_error("combi file not found");
        eng.loadCombi(hs2::Combi::load(path));
    } else {
        if (patch == nullptr) throw std::runtime_error("patch not read");
        eng.loadPatch(0, *patch, ref.bank);
    }

    struct Ev { juce::int64 frame; uint8_t status, d1, d2; };
    std::vector<Ev> evs;
    for (const auto& n : notes) {
        evs.push_back({juce::int64(n.start * kPreviewRate), 0x90, uint8_t(n.pitch), uint8_t(n.velocity)});
        evs.push_back({juce::int64((n.start + n.length) * kPreviewRate), 0x80, uint8_t(n.pitch), 0});
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });

    constexpr int kBlock = 512;
    const int total = int(seconds * kPreviewRate);
    juce::AudioBuffer<float> out(2, total);
    std::vector<float> lr(size_t(kBlock) * 2);
    std::vector<hs2::MidiEvent> block;
    hs2::HostTransport tr;
    tr.playing = true;
    // The loaded part is used from the next block: one silent block first.
    std::fill(lr.begin(), lr.end(), 0.f);
    eng.processBlock(lr.data(), kBlock, nullptr, 0, &tr);
    size_t next = 0;
    for (int pos = 0; pos < total; pos += kBlock) {
        const int n = std::min(kBlock, total - pos);
        block.clear();
        for (; next < evs.size() && evs[next].frame < pos + n; ++next) {
            hs2::MidiEvent m;
            m.offset = int(std::max<juce::int64>(0, evs[next].frame - pos));
            m.data[0] = evs[next].status;
            m.data[1] = evs[next].d1;
            m.data[2] = evs[next].d2;
            block.push_back(m);
        }
        tr.ppq = (pos + kBlock) / kPreviewRate * 2.0;  // quarter notes at 120 bpm (after the silent block)
        tr.barStart = std::floor(tr.ppq / 4.0) * 4.0;
        std::fill(lr.begin(), lr.begin() + 2 * n, 0.f);
        eng.processBlock(lr.data(), n, block.data(), int(block.size()), &tr);
        for (int i = 0; i < n; ++i) {
            out.setSample(0, pos + i, lr[size_t(2 * i)]);
            out.setSample(1, pos + i, lr[size_t(2 * i + 1)]);
        }
    }
    return out;
}

float rms(const juce::AudioBuffer<float>& b, int start, int n) {
    n = std::min(n, b.getNumSamples() - start);
    return n > 0 ? std::max(b.getRMSLevel(0, start, n), b.getRMSLevel(1, start, n)) : 0.f;
}

// For a preset that is silent with its phrase (drum "menus" and percussion mapped outside the phrase's keys): plays
// every key from 21 to 108 briefly, one after the other, and makes a phrase of up to eight of the keys that sound.
std::vector<PreviewNote> probePhrase(hs2::Content& content, const PatchRef& ref, const hs2::Patch* patch) {
    constexpr double step = 0.05;
    std::vector<PreviewNote> probe;
    for (int k = 21; k <= 108; ++k) probe.push_back({(k - 21) * step, step * 0.8, k, 110});
    const auto audio = renderNotes(content, ref, patch, probe, (108 - 21 + 1) * step + 0.5);
    std::vector<std::pair<int, float>> level;  // key, onset level
    float loudest = 0.f;
    for (const auto& n : probe) {
        const float l = rms(audio, int(n.start * kPreviewRate), int(step * kPreviewRate));
        level.push_back({n.pitch, l});
        loudest = std::max(loudest, l);
    }
    std::vector<int> keys;
    for (const auto& [k, l] : level)
        if (loudest > 1e-5f && l > loudest * 0.1f) keys.push_back(k);
    std::vector<PreviewNote> phrase;
    if (keys.empty()) return phrase;
    const int count = std::min<int>(8, int(keys.size()));
    for (int i = 0; i < count; ++i) {
        const int k = keys[size_t(count == 1 ? 0 : i * (int(keys.size()) - 1) / (count - 1))];
        phrase.push_back({i * 0.25, 0.2, k, i % 2 == 0 ? 110 : 90});
    }
    phrase.push_back({count * 0.25, 0.6, phrase.front().pitch, 110});
    return phrase;
}

// The preview: the phrase for the preset's sound type (or, when that is silent, one on the keys that sound),
// normalised and faded out.
juce::AudioBuffer<float> renderPreview(hs2::Content& content, const PatchRef& ref, const hs2::Patch* patch) {
    auto out = renderNotes(content, ref, patch, previewPhrase(ref), kPreviewSeconds);
    const int total = out.getNumSamples();
    if (rms(out, 0, total) < 1e-3f)
        if (const auto phrase = probePhrase(content, ref, patch); !phrase.empty())
            out = renderNotes(content, ref, patch, phrase, kPreviewSeconds);
    const float peak = out.getMagnitude(0, total);
    if (peak > 1e-6f) out.applyGain(std::min(kPreviewPeak / peak, kPreviewMaxGain));
    const int fade = int(kPreviewFade * kPreviewRate);
    out.applyGainRamp(total - fade, fade, 1.f, 0.f);
    return out;
}

bool writeOgg(const juce::File& file, const juce::AudioBuffer<float>& audio) {
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr) return false;
    juce::OggVorbisAudioFormat ogg;
#if JUCE_MAJOR_VERSION >= 8
    auto writer = ogg.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(kPreviewRate)
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(16)
                                                  .withQualityOptionIndex(2));  // 96 kbps
#else  // JUCE 7: the writer takes the stream only when it is created
    std::unique_ptr<juce::AudioFormatWriter> writer(
        ogg.createWriterFor(stream.get(), kPreviewRate, 2, 16, {}, 2));  // 96 kbps
    if (writer != nullptr) stream.release();
#endif
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

// One output format: its root, the files written this time, and the receipt of the last run.
struct Target {
    Target(const juce::File& destination, const char* suffix) : root(destination), extension(suffix) {}

    juce::File root;
    juce::String extension;
    juce::File stage;
    juce::StringArray written;                         // relative paths
    std::set<juce::String> previous;
    int previousMapping = 0;
    std::map<juce::String, std::set<juce::String>> used;  // folder -> lower-case file names taken this run
    juce::File receipt() const { return root.getChildFile(".hs2port-" + extension.substring(1) + "-presets.json"); }
    juce::String relative(const juce::File& file) const {
        return file.getRelativePathFrom(root).replaceCharacter('\\', '/');
    }
    juce::File staged(const juce::File& file) const { return stage.getChildFile("new/" + relative(file) + ".stage"); }

    bool contains(const juce::File& file) const {
        std::error_code ec;
        const auto base = std::filesystem::weakly_canonical(nativePath(root), ec);
        if (ec) return false;
        const auto resolved = std::filesystem::weakly_canonical(nativePath(file), ec);
        if (ec) return false;
        const auto relative = resolved.lexically_relative(base);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != ".." && relative != ".";
    }

    juce::File fileFor(const juce::String& folder, const juce::String& name) {
        auto& taken = used[folder];
        juce::String base = name, candidate = name;
        const auto available = [&](const juce::String& stem) {
            const auto rel = folder + "/" + stem + extension;
            const auto preview = folder + "/.previews/" + stem + extension + ".ogg";
            const auto free = [&](const juce::String& path) {
                return previous.contains(path) || !occupied(root.getChildFile(path));
            };
            return free(rel) && (extension != ".nksf" || free(preview));
        };
        for (int n = 2; taken.count(candidate.toLowerCase()) != 0 ||
                        !available(candidate); ++n)
            candidate = base + " (" + juce::String(n) + ")";
        taken.insert(candidate.toLowerCase());
        return root.getChildFile(folder + "/" + candidate + extension);
    }

    bool readPrevious() {
        auto manifest = receipt();
        if (!occupied(manifest)) manifest = root.getChildFile(kReceipt);
        if (!occupied(manifest)) return true;
        if (!manifest.existsAsFile() || manifest.isSymbolicLink() || !contains(manifest)) return false;
        const auto json = juce::JSON::parse(manifest);
        const auto* files = json["files"].getArray();
        if (json["product"].toString() != kPluginName || int(json["formatVersion"]) != kReceiptVersion || files == nullptr)
            return false;
        previousMapping = int(json["mappingRevision"]);
        for (const auto& f : *files) {
            const auto rel = f.toString().replaceCharacter('\\', '/');
            if (rel.isEmpty() || juce::File::isAbsolutePath(rel)) continue;
            const auto file = root.getChildFile(rel);
            const bool preview = extension == ".nksf" && file.getFileName().endsWithIgnoreCase(extension + ".ogg")
                                 && file.getParentDirectory().getFileName() == ".previews";
            if (relative(file) == rel && file.isAChildOf(root) && (file.hasFileExtension(extension) || preview)
                && file.existsAsFile() && !file.isSymbolicLink() && contains(file))
                previous.insert(rel);
        }
        return true;
    }

    bool writeReceipt() const {
        juce::Array<juce::var> files;
        for (const auto& w : written) files.add(w);
        const auto json = obj({{"product", kPluginName}, {"formatVersion", kReceiptVersion},
                               {"mappingRevision", kNksMappingRevision}, {"files", juce::var(files)}});
        return staged(receipt()).replaceWithText(juce::JSON::toString(json));
    }
};

struct PresetTransaction {
    struct Move { juce::File from, to; };
    std::vector<Move> moved;
    std::vector<juce::File> stages, createdDirs;
    bool keepRecovery = false;

    ~PresetTransaction() {
        if (!keepRecovery)
            for (const auto& stage : stages) stage.deleteRecursively(false);
    }

    bool makeLiveDirectory(const juce::File& root, const juce::File& dir) {
        if (dir == root) return dir.isDirectory();
        if (!dir.isAChildOf(root) || dir.isSymbolicLink() || !makeLiveDirectory(root, dir.getParentDirectory())) return false;
        if (dir.isDirectory()) return true;
        if (!dir.createDirectory()) return false;
        createdDirs.push_back(dir);
        return true;
    }

    bool move(const juce::File& from, const juce::File& to) {
        moved.push_back({from, to});
        if (moveWithoutReplacing(from, to)) return true;
        moved.pop_back();
        return false;
    }

    void rollback(juce::StringArray& errors) {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it)
            if (!moveWithoutReplacing(it->to, it->from)) {
                errors.add("Could not restore " + it->from.getFullPathName() + " from " + it->to.getFullPathName());
                keepRecovery = true;
            }
        for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it)
            if (it->getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0) it->deleteFile();
        if (keepRecovery)
            for (const auto& stage : stages) errors.add("Recovery files retained in " + stage.getFullPathName());
        else
            errors.add("Previous preset files restored.");
    }

    bool install(Target& target, juce::StringArray& errors) {
        const auto fail = [&](const juce::File& file) {
            errors.add("Cannot replace presets at " + file.getFullPathName());
            return false;
        };
        const auto save = [&](const juce::File& file) {
            const auto backup = target.stage.getChildFile("backup/" + target.relative(file) + ".stage");
            return target.contains(file) && !file.isSymbolicLink() && file.existsAsFile()
                   && backup.getParentDirectory().createDirectory() && move(file, backup);
        };
        for (const auto& rel : target.previous) {
            const auto file = target.root.getChildFile(rel);
            if (occupied(file) && !save(file)) return fail(file);
        }
        const auto receipt = target.receipt();
        if (occupied(receipt) && !save(receipt)) return fail(receipt);
        for (const auto& rel : target.written) {
            const auto file = target.root.getChildFile(rel);
            if (!target.contains(file) || !makeLiveDirectory(target.root, file.getParentDirectory())
                || !move(target.staged(file), file)) return fail(file);
        }
        return move(target.staged(receipt), receipt) || fail(receipt);
    }
};

}  // namespace

juce::MemoryBlock patchPresetState(const PatchRef& ref) {
    juce::XmlElement xml("HS2Port");
    xml.setAttribute("stateVersion", 2);
    xml.setAttribute("preset", ref.combi ? "combi" : "patch");
    auto* setup = xml.createNewChildElement("SETUP");
    if (ref.combi) {
        setup->setAttribute("combiBank", ref.bank);
        setup->setAttribute("combiIndex", ref.index);
        setup->setAttribute("combiName", latin1(ref.name));
    } else {
        auto* e = setup->createNewChildElement("PART");
        e->setAttribute("part", 1);
        e->setAttribute("kind", "patch");
        e->setAttribute("bank", ref.bank);
        e->setAttribute("patch", ref.index);
        e->setAttribute("name", latin1(ref.name));
    }
    juce::MemoryBlock mb;
    juce::AudioProcessor::copyXmlToBinary(xml, mb);
    return mb;
}

juce::MemoryBlock makeVstPreset(const juce::MemoryBlock& componentState) {
    juce::MemoryOutputStream o;
    o.write("VST3", 4);
    o.writeInt(1);                      // format version
    o.write(kVst3ClassId, 32);          // ASCII class ID
    const juce::int64 dataStart = 48;   // header size
    const juce::int64 listOffset = dataStart + juce::int64(componentState.getSize());
    o.writeInt64(listOffset);
    o.write(componentState.getData(), componentState.getSize());
    o.write("List", 4);
    o.writeInt(1);
    o.write("Comp", 4);
    o.writeInt64(dataStart);
    o.writeInt64(juce::int64(componentState.getSize()));
    return o.getMemoryBlock();
}

juce::String makeAuPreset(const juce::MemoryBlock& state, const juce::String& name) {
    juce::String s;
    s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n<dict>\n"
      << "\t<key>jucePluginState</key>\n\t<data>" << juce::Base64::toBase64(state.getData(), state.getSize())
      << "</data>\n"
      << "\t<key>manufacturer</key>\n\t<integer>" << kAuManufacturer << "</integer>\n"
      << "\t<key>name</key>\n\t<string>"
      << name.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;") << "</string>\n"
      << "\t<key>subtype</key>\n\t<integer>" << kAuSubtype << "</integer>\n"
      << "\t<key>type</key>\n\t<integer>" << kAuType << "</integer>\n"
      << "\t<key>version</key>\n\t<integer>0</integer>\n"
      << "</dict>\n</plist>\n";
    return s;
}

juce::File defaultVst3PresetFolder() {
#if JUCE_MAC
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library/Audio/Presets").getChildFile(kVendor).getChildFile(kPluginName);
#else
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("VST3 Presets").getChildFile(kVendor).getChildFile(kPluginName);
#endif
}

juce::File defaultAuPresetFolder() {
#if JUCE_MAC
    return defaultVst3PresetFolder();  // ~/Library/Audio/Presets/<manufacturer>/<plugin>
#else
    return {};
#endif
}

juce::File defaultNksPresetFolder() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Native Instruments/User Content").getChildFile(kPluginName);
}

bool kompleteKontrolInstalled() {
#if JUCE_WINDOWS
    const auto dir = juce::WindowsRegistry::getValue(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Native Instruments\\Komplete Kontrol\\InstallDir");
    if (dir.isNotEmpty() && juce::File::isAbsolutePath(dir) &&
        juce::File(dir).getChildFile("Komplete Kontrol.exe").existsAsFile())
        return true;
    return juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
        .getChildFile("Native Instruments\\Komplete Kontrol\\Komplete Kontrol.exe")
        .existsAsFile();
#elif JUCE_MAC
    for (const auto* path : {"/Applications/Native Instruments/Komplete Kontrol/Komplete Kontrol.app",
                             "/Applications/Native Instruments/Komplete Kontrol.app",
                             "/Applications/Komplete Kontrol.app"})
        if (juce::File(path).exists()) return true;
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Applications/Native Instruments/Komplete Kontrol/Komplete Kontrol.app")
        .exists();
#else
    return false;
#endif
}

bool hasOlderNksPresets(const juce::File& root) {
    if (!root.isDirectory()) return false;
    Target nks(root, ".nksf");
    return nks.readPrevious() && nks.previousMapping < kNksMappingRevision
           && std::any_of(nks.previous.begin(), nks.previous.end(), [](const auto& path) {
               return path.endsWithIgnoreCase(".nksf");
           });
}

PresetExportResult exportPresets(const PatchLibrary& library, const PresetExportOptions& options,
                                 const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel) {
    PresetExportResult result;
    static std::mutex exportMutex;
    const std::unique_lock processLock(exportMutex, std::try_to_lock);
    juce::InterProcessLock exportLock("HS2PortPresetExport");
    if (!processLock.owns_lock() || !exportLock.enter(0)) {
        result.errors.add("Another preset installation is in progress.");
        return result;
    }
    if (!library.errors.empty()) {
        result.errors.add("The content library could not be read completely: " + juce::String(library.errors.front()));
        return result;
    }
    for (const auto& root : options.requiredExistingRoots) {
        if (!root.isDirectory()) {
            result.errors.add("Selected preset folder is unavailable: " + root.getFullPathName());
            return result;
        }
    }
    PresetTransaction transaction;
    std::vector<Target> targets;
    if (options.vst3Root != juce::File()) targets.emplace_back(options.vst3Root, ".vstpreset");
    if (options.auRoot != juce::File()) targets.emplace_back(options.auRoot, ".aupreset");
    if (options.nksRoot != juce::File()) targets.emplace_back(options.nksRoot, ".nksf");
    for (auto& t : targets) {
        if (!t.root.createDirectory()) {
            result.errors.add("Cannot create " + t.root.getFullPathName());
            return result;
        }
        for (const auto& dir : t.root.findChildFiles(juce::File::findDirectories, false, juce::String(kStagePrefix) + "*"))
            if (dir.getChildFile(kCommitMarker).exists()) {
                result.errors.add("An interrupted installation has recovery files in " + dir.getFullPathName());
                return result;
            }
        if (!t.readPrevious()) {
            result.errors.add("Cannot read the previous preset receipt in " + t.root.getFullPathName());
            return result;
        }
        // A staging directory inside the destination works for symlinked folders and volume roots too.
        t.stage = t.root.getChildFile(juce::String(kStagePrefix) + juce::Uuid().toString());
        if (occupied(t.stage) || !t.stage.getChildFile("new").createDirectory()) {
            result.errors.add("Cannot stage presets in " + t.root.getFullPathName());
            return result;
        }
        transaction.stages.push_back(t.stage);
    }

    const auto cancelled = [&] { return cancel != nullptr && cancel->load(); };

    std::map<int, int> lastProgram;
    for (const auto& ref : library.patches)
        lastProgram[ref.bank] = std::max(lastProgram[ref.bank], ref.index + 1);

    std::map<int, std::unique_ptr<hs2::MilanBank>> banks;  // patch banks opened so far (memory-mapped)
    // Previews: the engine's sample cache keeps every sample bank it loads, so a new one is made per patch bank
    // (and one for all combis) to keep memory down.
    std::unique_ptr<hs2::Content> previewContent;
    int previewContentKey = -1;
    const auto contentFor = [&](const PatchRef& ref) -> hs2::Content& {
        const int key = ref.combi ? 1000 : ref.bank;
        if (previewContent == nullptr || key != previewContentKey) {
            previewContent.reset();
            previewContent = std::make_unique<hs2::Content>(
                std::filesystem::path(options.contentRoot.getFullPathName().toWideCharPointer()));
            previewContentKey = key;
        }
        return *previewContent;
    };
    const int total = int(library.patches.size());
    for (int i = 0; i < total; ++i) {
        if (progress && (i % 50 == 0)) progress(i, total);
        if (cancelled()) break;
        const auto& ref = library.patches[size_t(i)];
        if (ref.copyProtected && !ref.captured) {
            ++result.skippedProtected;
            continue;
        }
        const auto* bank = library.bank(ref.bank);
        std::unique_ptr<hs2::Patch> patch;
        std::optional<hs2::Combi> combi;
        if (!ref.combi && bank != nullptr) {
            try {
                auto& mb = banks[ref.bank];
                if (mb == nullptr)
                    mb = hs2::MilanBank::open(std::filesystem::path(
                        options.contentRoot.getChildFile("Patches").getChildFile(latin1(bank->name))
                            .getFullPathName().toWideCharPointer()));
                const auto& e = mb->entries.at(size_t(ref.index));
                // A copy-protected patch: load its capture instead (the plain patch taken out of the licensed
                // original by hs2capture; PatchLibrary::scan found it in <content>/Captures).
                if (ref.captured) {
                    const auto* cap = library.capturePath(ref.bank, ref.index);
                    if (cap == nullptr) throw std::runtime_error("its capture file is missing");
                    std::ifstream f(*cap, std::ios::binary);
                    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)),
                                                    std::istreambuf_iterator<char>());
                    patch = std::make_unique<hs2::Patch>(hs2::Patch::parse(blob));
                } else {
                    patch = std::make_unique<hs2::Patch>(hs2::Patch::parse(mb->read(e.slot1)));
                }
            } catch (const std::exception& ex) {
                result.errors.add(latin1(ref.name) + ": " + ex.what());
                continue;
            }
        }
        if (ref.combi && options.nksRoot != juce::File()) {
            try {
                const auto root = std::filesystem::path(options.contentRoot.getFullPathName().toWideCharPointer());
                combi = hs2::Combi::load(hs2::combiPath(root, ref.bank, ref.index));
            } catch (const std::exception& ex) {
                result.errors.add(latin1(ref.name) + ": " + ex.what());
                continue;
            }
        }
        const auto state = patchPresetState(ref);
        const auto vst = makeVstPreset(state);
        const juce::String name = latin1(ref.name);
        const auto folder = juce::String(ref.combi ? "Combis/" : "Patches/") +
                            safeFileName(bank != nullptr ? latin1(bank->name) : juce::String(ref.bank), "Bank");
        const auto fileName = safeFileName(name, "Patch " + juce::String(ref.index + 1));
        bool ok = true;
        for (auto& t : targets) {
            const auto live = t.fileFor(folder, fileName);
            const auto file = t.staged(live);
            if (!file.getParentDirectory().createDirectory()) {
                result.errors.add("Cannot stage " + live.getFullPathName());
                ok = false;
                break;
            }
            if (t.extension == ".vstpreset") ok = file.replaceWithData(vst.getData(), vst.getSize()) && ok;
            else if (t.extension == ".aupreset") ok = file.replaceWithText(makeAuPreset(state, name)) && ok;
            else {
                const auto nks = makeNksPreset(ref, patch.get(), combi ? &*combi : nullptr, bank, vst,
                                               juce::String(lastProgram.at(ref.bank)).length() > 2 ? 3 : 2);
                ok = file.replaceWithData(nks.getData(), nks.getSize()) && ok;
                if (ok && options.nksPreviews) {
                    const auto dir = live.getParentDirectory().getChildFile(".previews");
                    const auto ogg = dir.getChildFile(live.getFileName() + ".ogg");
                    const auto stagedOgg = t.staged(ogg);
                    bool written = false;
                    juce::String why;
                    try {
                        const auto audio = renderPreview(contentFor(ref), ref, patch.get());
                        written = stagedOgg.getParentDirectory().createDirectory() && writeOgg(stagedOgg, audio);
                        if (!written) why = "cannot write " + ogg.getFullPathName();
                    } catch (const std::exception& ex) {
                        why = ex.what();
                    }
                    if (written) {
                        ++result.previews;
                        t.written.add(t.relative(ogg));
                    } else {
                        ++result.previewFailures;
                        result.errors.add("No preview for " + name + ": " + why);
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok) {
                result.errors.add("Cannot stage " + live.getFullPathName());
                break;
            }
            t.written.add(t.relative(live));
        }
        if (!ok) break;
        ++(ref.combi ? result.combis : result.patches);
    }
    if (progress) progress(total, total);
    if (cancelled()) result.errors.add("Cancelled before replacement.");
    if (result.errors.isEmpty() && result.patches + result.combis == 0 && !targets.empty())
        result.errors.add("No loadable presets found; nothing was replaced.");
    if (result.errors.isEmpty())
        for (auto& t : targets)
            if (!t.writeReceipt()) result.errors.add("Cannot stage the receipt in " + t.root.getFullPathName());
    if (result.errors.isEmpty())
        for (const auto& stage : transaction.stages)
            if (!stage.getChildFile(kCommitMarker).create())
                result.errors.add("Cannot prepare replacement in " + stage.getFullPathName());
    if (result.errors.isEmpty() && cancelled()) result.errors.add("Cancelled before replacement.");
    if (result.errors.isEmpty()) {
        // Once live moves begin, finish or roll back rather than leaving a partially replaced bank.
        for (auto& t : targets)
            if (!transaction.install(t, result.errors)) {
                transaction.rollback(result.errors);
                break;
            }
    }
    if (!result.errors.isEmpty()) result.patches = result.combis = result.previews = 0;
    return result;
}

}  // namespace hs2port
