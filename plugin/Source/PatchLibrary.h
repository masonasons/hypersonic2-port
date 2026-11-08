#pragma once
// The list of patches in the user's Hypersonic 2 content folder, for the browser and MIDI program changes.
// Only names are kept: a scan opens each patch bank once and reads its directory.
#include <filesystem>
#include <string>
#include <vector>

namespace hs2port {

struct PatchRef {
    int bank = -1;          // bank number (the NNN file prefix; MIDI bank select uses it too)
    int index = -1;         // entry index inside the bank (MIDI program number)
    std::string name;       // patch name as stored in the bank directory (combis: from the file name)
    bool combi = false;     // a combi (a whole 16-part setup): bank 120 and up, index = the file's number prefix
};

struct BankInfo {
    int number = -1;
    std::string name;       // file name, e.g. "015 Synth Basses"
    std::vector<int> patches;  // indices into PatchLibrary::patches
    bool combi = false;     // a combi bank folder ("120 Combis", "121 User Combis")
};

class PatchLibrary {
public:
    // Scans <root>/Patches: the patch bank files and the combi bank folders (Patches/NNN name/MMM name.fxb).
    // Never throws; a bank that fails to open is reported in `errors`.
    static PatchLibrary scan(const std::filesystem::path& root);

    std::vector<PatchRef> patches;
    std::vector<BankInfo> banks;
    std::vector<std::string> errors;

    const BankInfo* bank(int number) const;
    // Index into `patches`, or -1.
    int find(int bank, int index) const;
    int findByName(int bank, const std::string& name) const;
};

// A patch name as shown to the user: the one-character marker many names start with ('[' ']' '{' '}' '_' or a
// space; meaning not known yet, probably grouping in the original browser) and surrounding spaces removed.
std::string displayName(const std::string& raw);

// Case-insensitive match of every space-separated word in `query` against `text`.
bool matchesQuery(const std::string& text, const std::string& query);

}  // namespace hs2port
