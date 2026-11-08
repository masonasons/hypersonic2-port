#pragma once
// Hypersonic 2 combi: a whole 16-part setup, stored as a VST 2 bank (.fxb, fxID 'StSS') whose chunk is an 'StSS'
// bank chunk ('B', 1 at +0xc) read by the plugin's setChunk (FUN_1008c270). Layout: docs/NOTES.md "Combi format",
// tools/formats/combi.py. Factory combis: "Patches/120 Combis/*.fxb" (bank 120; user combi banks 121..128).
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "hs2/patch.h"

namespace hs2 {

struct Combi {
    uint32_t version = 0;                      // chunk +4 (accepted 0x990EDF .. 0xD1CEF00)
    std::array<std::optional<Patch>, 16> parts;  // the part patches (their params carry the part settings)
    std::array<PatchRecord, 4> globalFx;       // the global send FX element records (setter elements 0x41..0x44)
    uint32_t settingsVersion = 0;
    float masterVolume = 0.5f;                 // VST parameter 0
    std::array<uint8_t, 16> links{};           // links[k] (k = 1..15): part k plays the events of part k - 1
    std::string name;                          // settings version > 0x33
    bool hasSetup = false;                     // settings version > 0x3f: vel / touch sens, memory mode
    float velSens = 0.5f, touchSens = 0.5f, memory = 0.5f;
    bool hasTune = false;                      // settings version > 0x4f: master tune, hype, GM mode
    int tuneSemis = 0;
    float tuneFine = 0.f, hype = 0.33f;
    uint32_t gm = 0;
    std::vector<int32_t> chain;                // settings version > 0x5f: combi chain (128 x bank * 256 + program)

    // A .fxb file's bytes or a raw chunk. Throws std::runtime_error.
    static Combi parse(std::span<const uint8_t> data);
    static Combi load(const std::filesystem::path& file);
};

// Combi Chain (.h2c): "HS2//COMBICHAIN" then 128 lines of bank * 256 + program (-1 = empty).
std::vector<int32_t> parseCombiChain(const std::string& text);

// The factory / user combi file of combi bank `bank` (120..128) entry `index`: the index-th "NNN *.fxb" file of
// "Patches/<bank> ..." by its number prefix; empty if absent.
std::filesystem::path combiPath(const std::filesystem::path& contentRoot, int bank, int index);

}  // namespace hs2
