#include "PresetExport.h"

#include <map>
#include <memory>
#include <set>

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

juce::String latin1(const std::string& s) {
    juce::String out;
    out.preallocateBytes(s.size() * 2);
    for (unsigned char c : s) out += juce::juce_wchar(c);
    return out;
}

juce::String bankTitle(const BankInfo* b) {
    if (b == nullptr) return {};
    const auto n = latin1(b->name);
    return n.length() > 4 && juce::CharacterFunctions::isDigit(n[0]) ? n.substring(4) : n;
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
juce::var nksKnobPages(const PatchRef& ref, const hs2::Patch* patch) {
    if (ref.combi) {
        juce::Array<juce::var> p1, p2;
        for (int part = 0; part < 8; ++part)
            p1.add(knob(exposedIndex(part, ppLevel), "Part " + juce::String(part + 1), part == 0 ? "Part levels" : ""));
        for (int part = 8; part < 16; ++part)
            p2.add(knob(exposedIndex(part, ppLevel), "Part " + juce::String(part + 1), part == 8 ? "Part levels" : ""));
        return obj({{"ni8", arr({juce::var(p1), juce::var(p2)})}});
    }
    juce::Array<juce::var> p1, p2;
    for (int k = 0; k < kNumKnobs; ++k) {
        juce::String n = patch != nullptr ? latin1(patch->quickControls[size_t(k)]).trim() : juce::String();
        if (n.isEmpty()) n = "Hyperknob " + juce::String(k + 1);
        p1.add(knob(exposedIndex(0, ppKnob1 + k), n, k == 0 ? "Hyperknobs" : ""));
    }
    p1.add(knob(exposedIndex(0, ppLevel), "Level", "Mixer"));
    p1.add(knob(exposedIndex(0, ppPan), "Pan"));
    for (int s = 0; s < kNumSends; ++s)
        p2.add(knob(exposedIndex(0, ppSend1 + s), "Send " + juce::String(s + 1), s == 0 ? "Effect sends" : ""));
    p2.add(knob(exposedIndex(0, ppLevel), "Level", "Mixer"));
    p2.add(knob(exposedIndex(0, ppPan), "Pan"));
    p2.add(knob(exposedIndex(0, ppMute), "Mute"));
    p2.add(knob(0, "Master"));
    return obj({{"ni8", arr({juce::var(p1), juce::var(p2)})}});
}

juce::MemoryBlock makeNksPreset(const PatchRef& ref, const hs2::Patch* patch, const BankInfo* bank,
                                const juce::MemoryBlock& vstPreset) {
    const juce::String name = latin1(ref.name);
    const auto nisi = obj({{"author", patch != nullptr ? latin1(patch->author).trim() : juce::String()},
                           {"bankchain", arr({juce::String(kPluginName), bankTitle(bank), juce::String()})},
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
    riffChunk(body, "NICA", nksMsgpack(nksKnobPages(ref, patch)));
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
    auto writer = ogg.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(kPreviewRate)
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(16)
                                                  .withQualityOptionIndex(2));  // 96 kbps
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

// One output format: its root, the files written this time, and the receipt of the last run.
struct Target {
    juce::File root;
    juce::String extension;
    juce::StringArray written;                         // relative paths
    std::map<juce::String, std::set<juce::String>> used;  // folder -> lower-case file names taken this run

    juce::File fileFor(const juce::String& folder, const juce::String& name) {
        auto& taken = used[folder];
        juce::String base = name, candidate = name;
        for (int n = 2; taken.count(candidate.toLowerCase()) != 0; ++n) candidate = base + " (" + juce::String(n) + ")";
        taken.insert(candidate.toLowerCase());
        const auto rel = folder + "/" + candidate + extension;
        written.add(rel);
        return root.getChildFile(rel);
    }

    // Removes the files the previous run wrote (only those) and the folders that are left empty.
    void removePrevious() {
        const auto receipt = root.getChildFile(kReceipt);
        if (!receipt.existsAsFile()) return;
        const auto json = juce::JSON::parse(receipt);
        if (const auto* files = json["files"].getArray()) {
            std::set<juce::File> dirs;
            for (const auto& f : *files) {
                const auto rel = f.toString();
                if (rel.isEmpty() || rel.contains("..") || juce::File::isAbsolutePath(rel)) continue;
                const auto file = root.getChildFile(rel);
                const bool preview = file.getFileName().endsWithIgnoreCase(extension + ".ogg");  // .previews/x.nksf.ogg
                if (file.isAChildOf(root) && (file.hasFileExtension(extension) || preview)) {
                    file.deleteFile();
                    dirs.insert(file.getParentDirectory());
                }
            }
            for (auto d : dirs)
                for (; d.isAChildOf(root) && d.isDirectory() && d.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0;
                     d = d.getParentDirectory())
                    d.deleteFile();
        }
    }

    bool writeReceipt() const {
        juce::Array<juce::var> files;
        for (const auto& w : written) files.add(w);
        const auto json = obj({{"product", kPluginName}, {"formatVersion", 1}, {"files", juce::var(files)}});
        return root.getChildFile(kReceipt).replaceWithText(juce::JSON::toString(json));
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

PresetExportResult exportPresets(const PatchLibrary& library, const PresetExportOptions& options,
                                 const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel) {
    PresetExportResult result;
    std::vector<Target> targets;
    if (options.vst3Root != juce::File()) targets.push_back({options.vst3Root, ".vstpreset", {}, {}});
    if (options.auRoot != juce::File()) targets.push_back({options.auRoot, ".aupreset", {}, {}});
    if (options.nksRoot != juce::File()) targets.push_back({options.nksRoot, ".nksf", {}, {}});
    for (auto& t : targets) {
        if (!t.root.createDirectory()) {
            result.errors.add("Cannot create " + t.root.getFullPathName());
            return result;
        }
        t.removePrevious();
    }

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
        if (cancel != nullptr && cancel->load()) {
            result.errors.add("Cancelled.");
            break;
        }
        if (progress && (i % 50 == 0)) progress(i, total);
        const auto& ref = library.patches[size_t(i)];
        if (ref.copyProtected) {
            ++result.skippedProtected;
            continue;
        }
        const auto* bank = library.bank(ref.bank);
        std::unique_ptr<hs2::Patch> patch;
        if (!ref.combi && bank != nullptr) {
            try {
                auto& mb = banks[ref.bank];
                if (mb == nullptr)
                    mb = hs2::MilanBank::open(std::filesystem::path(
                        options.contentRoot.getChildFile("Patches").getChildFile(latin1(bank->name))
                            .getFullPathName().toWideCharPointer()));
                const auto& e = mb->entries.at(size_t(ref.index));
                patch = std::make_unique<hs2::Patch>(hs2::Patch::parse(mb->read(e.slot1)));
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
            const auto file = t.fileFor(folder, fileName);
            if (!file.getParentDirectory().createDirectory()) ok = false;
            if (t.extension == ".vstpreset") ok = file.replaceWithData(vst.getData(), vst.getSize()) && ok;
            else if (t.extension == ".aupreset") ok = file.replaceWithText(makeAuPreset(state, name)) && ok;
            else {
                const auto nks = makeNksPreset(ref, patch.get(), bank, vst);
                ok = file.replaceWithData(nks.getData(), nks.getSize()) && ok;
                if (ok && options.nksPreviews) {
                    const auto dir = file.getParentDirectory().getChildFile(".previews");
                    const auto ogg = dir.getChildFile(file.getFileName() + ".ogg");
                    bool written = false;
                    juce::String why;
                    try {
                        const auto audio = renderPreview(contentFor(ref), ref, patch.get());
                        written = dir.createDirectory() && writeOgg(ogg, audio);
                        if (!written) why = "cannot write " + ogg.getFullPathName();
                    } catch (const std::exception& ex) {
                        why = ex.what();
                    }
                    if (written) {
                        ++result.previews;
                        t.written.add(ogg.getRelativePathFrom(t.root).replaceCharacter('\\', '/'));
                    } else if (++result.previewFailures <= 5) {
                        result.errors.add("No preview for " + name + ": " + why);
                    }
                }
            }
            if (!ok) {
                result.errors.add("Cannot write " + file.getFullPathName());
                break;
            }
        }
        if (!ok) break;
        ++(ref.combi ? result.combis : result.patches);
    }
    for (auto& t : targets)
        if (!t.writeReceipt()) result.errors.add("Cannot write the receipt in " + t.root.getFullPathName());
    if (progress) progress(total, total);
    return result;
}

}  // namespace hs2port
