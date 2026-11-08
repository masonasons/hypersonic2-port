#pragma once
// Hypersonic 2 patch ("StSS" blob). Layout: docs/NOTES.md, tools/formats/patch.py (FUN_10016f70).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hs2 {

enum class ElementType : int { Effect = 0, Drawbar = 1, Sample = 2, Wavetable = 3, FM = 4, Analog = 5, Model = 6 };

using ParamArray = std::array<float, 256>;

struct PatchRecord {
    std::string name;
    ParamArray params{};
};

struct Patch {
    uint32_t version = 0;
    std::array<uint8_t, 4> sendFxFlags{};
    std::string name, shortName, description, controllers, author;
    std::array<std::string, 6> quickControls;
    ParamArray params{};
    std::vector<PatchRecord> elements;  // inserts (type Effect) included
    std::vector<PatchRecord> sendFx;
    // after the records (FUN_10016f70 @100174d8): marker 0 = no CC map (the Hyperknob CCs come from patch params
    // 0x04, 0x05, 0x68, 0x6a, 0x6c, 0x6e), 2 = a CC map of 128 shorts (part+0x1a0) follows (then custom phrases)
    uint32_t tailMarker = 0;
    std::vector<int16_t> ccMap;
    std::vector<uint8_t> blob;  // the StSS blob itself (embedded phrases: Arp::patchTrailer)

    static Patch parse(std::span<const uint8_t> blob);  // throws std::runtime_error
};

// Hypersonic converts every enum with a truncating float->int (the compiler sets the FPU to chop).
inline int quant(float v, float k) { return int(v * k); }

inline ElementType elementType(const PatchRecord& r) { return ElementType(quant(r.params[1], 15.9f)); }

}  // namespace hs2
