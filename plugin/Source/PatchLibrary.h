#pragma once
// The list of patches in the user's Hypersonic 2 content folder, for the browser and MIDI program changes.
// Only names are kept: a scan opens each patch bank once and reads its directory.
#include <cstdint>
#include <cstring>
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace hs2port {

struct PatchRef {
    int bank = -1;          // bank number (the NNN file prefix; MIDI bank select uses it too)
    int index = -1;         // entry index inside the bank (MIDI program number)
    std::string name;       // patch name as stored in the bank directory (combis: from the file name)
    bool combi = false;     // a combi (a whole 16-part setup): bank 120 and up, index = the file's number prefix
    // A Syncrosoft copy-protected patch (the stored data starts with "SyncrosoftContent" instead of "StSS"; 63 of
    // the factory patches, the "New" HS-2 additions). The port cannot read the stored form; a capture (below)
    // is the plain patch taken out of the licensed original by hs2capture (tools/hs2capture), and loads normally.
    bool copyProtected = false;
    bool captured = false;  // copy-protected, and a plain capture file exists: loadable
};

// True for stored patch data that is Syncrosoft copy-protected.
inline bool isCopyProtectedPatch(const uint8_t* data, size_t size) {
    static const char kTag[] = "Syncrosoft";
    return size >= sizeof(kTag) - 1 && std::memcmp(data, kTag, sizeof(kTag) - 1) == 0;
}

struct BankInfo {
    int number = -1;
    std::string name;       // file name, e.g. "015 Synth Basses"
    std::vector<int> patches;  // indices into PatchLibrary::patches
    bool combi = false;     // a combi bank folder ("120 Combis", "121 User Combis")
};

// A microtuning bank (Tunings/NNN name): its number (a part's Tuning Bank setting) and its scales' names.
struct TuningBank {
    int number = -1;
    std::string name;                 // file name, e.g. "000 Microtunings"
    std::vector<std::string> scales;  // entry names (a part's Tuning Scale setting is the index)
};

// An effect preset (Effects/<category>/<name>.fxp, 1252 bytes): what the original's FUN_10085160 reads from it.
struct EffectPreset {
    std::filesystem::path rel;      // under Effects, e.g. "Delay/Mono Delay.fxp"
    float type = 1.f;               // element param 0x13 (file +0x130): the effect type
    std::array<float, 12> knobs{};  // element params 0x19..0x24 (file +0x148)
    static bool read(const std::filesystem::path& file, EffectPreset& out);
};

class PatchLibrary {
public:
    // Scans <root>/Patches: the patch bank files and the combi bank folders (Patches/NNN name/MMM name.fxb).
    // It also scans <root>/Captures (and <root>/Captures/patches, where hs2capture --out "<root>\Captures"
    // writes): the plain "StSS" files of the copy-protected factory patches, taken out of the licensed
    // original by hs2capture (tools/hs2capture, --mode patch). Those patches then load like any other.
    // Never throws; a bank that fails to open is reported in `errors`.
    static PatchLibrary scan(const std::filesystem::path& root);

    std::vector<PatchRef> patches;
    std::vector<BankInfo> banks;
    std::vector<TuningBank> tunings;
    std::vector<EffectPreset> effects;  // sorted by folder (User Effects last), then name
    const TuningBank* tuning(int number) const;
    // The sample bank and wavetable bank files by number (0..128): "042 Pianos", empty where there is none (the
    // element editor names a sample or wavetable element's bank by these).
    std::vector<std::string> sampleBankNames = std::vector<std::string>(129);
    std::vector<std::string> wavetableNames = std::vector<std::string>(129);
    // The waves of wavetable bank n by name, in entry order (empty when the bank has none or could not be read).
    std::vector<std::vector<std::string>> wavetableWaves = std::vector<std::vector<std::string>>(129);
    std::vector<std::string> errors;
    // (bank, index) -> the .stss capture file, for the copy-protected patches that have one (PatchRef::captured).
    std::map<std::pair<int, int>, std::filesystem::path> captured;

    const BankInfo* bank(int number) const;
    // Index into `patches`, or -1.
    int find(int bank, int index) const;
    int findByName(int bank, const std::string& name) const;
    const std::filesystem::path* capturePath(int bank, int index) const {
        const auto it = captured.find({bank, index});
        return it == captured.end() ? nullptr : &it->second;
    }
};

// A patch name as shown to the user: the one-character marker many names start with ('[' ']' '{' '}' '_' or a
// space; meaning not known yet, probably grouping in the original browser) and surrounding spaces removed.
std::string displayName(const std::string& raw);

// Case-insensitive match of every space-separated word in `query` against `text`.
bool matchesQuery(const std::string& text, const std::string& query);

}  // namespace hs2port
