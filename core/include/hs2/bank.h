#pragma once
// Wizoo "Milan Bank" container: every Hypersonic 2 content bank (patches, samples, wavetables, ...).
// Layout and semantics: docs/NOTES.md, tools/formats/milanbank.py (FUN_10019290 / FUN_1001a700).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hs2 {

struct BankSlot {
    uint32_t flag = 0, offset = 0, size = 0;
};

struct BankEntry {
    int index = 0;
    std::string name, name2;
    BankSlot slot1, slot2;
};

class MilanBank {
public:
    static std::unique_ptr<MilanBank> open(const std::filesystem::path& path);  // throws std::runtime_error

    char letter = 0;       // S samples, P patches, H phrases, W wavetables, D drawbars, T tunings
    uint16_t kind = 0;     // P=1 S=2 W=3 T=4 D=5 H=6
    uint16_t version = 0;  // 13 = masked offsets/sizes
    std::vector<BankEntry> entries;
    std::vector<BankSlot> records;  // secondary table (sample data in S banks); [0] is a sentinel

    // The file is memory-mapped (read-only): a read touches only the pages it needs, nothing is copied, and the OS
    // keeps the pages cached. Sample banks are up to 180 MB; reading them whole made a patch's first load slow.
    std::span<const uint8_t> read(const BankSlot& s) const;
    std::span<const uint8_t> bytes() const { return {data_, size_}; }
    const std::filesystem::path& path() const { return path_; }

private:
    std::shared_ptr<const void> map_;  // owns the mapping (unmapped when the bank goes)
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::filesystem::path path_;
};

inline uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
float rdf(const uint8_t* p);

}  // namespace hs2
