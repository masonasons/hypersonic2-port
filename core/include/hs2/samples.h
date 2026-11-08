#pragma once
// Sample banks (S): instruments -> zones -> sample blocks (16-bit PCM or Wizoo's compressed codec).
// Formats: extract script/milan_decode.py, docs/engine/sample_voice.md.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hs2/bank.h"

namespace hs2 {

// 0x40-byte zone record (engine's zone pointer = slot1 + header skip + 0x40 * k).
struct Zone {
    uint8_t velLo = 0, keyLo = 0, velHi = 127, keyHi = 127;
    uint32_t rootFlags = 60;  // +4: root key in bits 0..6, 0x200 = root follows the element key range
    uint32_t sample = 0;      // +8: 1-based secondary record index
    uint32_t frames = 0;      // +0x10
    uint32_t loopLen = 0;     // +0x14: loop = last loopLen frames
    float tune = 0;           // +0x18: semitones (playback correction)
    float loopTune = 0;       // +0x1c: semitones applied after the first loop pass (v[0xf])
    float pan = 0;            // +0x20: added to the voice pan
    float volume = 1;         // +0x24: linear gain
    int root() const { return int(rootFlags & 0x7f); }
    // the record's first word as the lookup compares it (FUN_1001b2a0: signed)
    int32_t order() const { return int32_t(uint32_t(keyHi) << 24 | uint32_t(velHi) << 16 | uint32_t(keyLo) << 8 | velLo); }
};

struct Instrument {
    std::string name;
    std::vector<Zone> zones;     // slot 1: XXL (full) zone set, every record (sorted, as stored)
    std::vector<Zone> ecoZones;  // slot 2: ECO zone set, present for ~9 % of instruments (FUN_10019c20)
};

struct SampleData {
    int channels = 1;
    int rate = 44100;
    int format = 0;           // 1 compressed codec, 2/4 PCM
    uint32_t frames = 0;      // frames stored in `data`
    std::vector<float> data;  // interleaved, 1.0 = full scale, plus guard frames
    // Compressed (format 1) only: the per-frame decoded deltas the render loop adds (interleaved, + guard frames),
    // and the 64 seek checkpoints' decoder values (block +0x10 + 4 (ch i + c)); checkpoint 0 is the loop start
    // (FUN_1004aad0 restores it at every loop wrap).
    std::vector<float> delta;
    std::vector<float> checkpoint;
};

class SampleBank {
public:
    static std::unique_ptr<SampleBank> open(const std::filesystem::path& path);
    std::vector<Instrument> instruments;
    // Decoded on first use and kept; nullptr if missing/unsupported. Thread-safe (the decode runs outside the lock;
    // two threads decoding the same sample at once keep the first result).
    const SampleData* sample(uint32_t index);
    const MilanBank& bank() const { return *bank_; }
    size_t sampleCount() const { return bank_->records.size(); }  // secondary records (0 = sentinel)

private:
    std::unique_ptr<MilanBank> bank_;
    std::map<uint32_t, std::unique_ptr<SampleData>> cache_;  // guarded by mu_ (nodes never move)
    std::mutex mu_;
};

// Decoders (exposed for tests).
bool decodeSampleBlock(const uint8_t* block, size_t size, SampleData& out);

}  // namespace hs2
