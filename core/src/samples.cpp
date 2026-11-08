#include "hs2/samples.h"

#include <cstring>

namespace hs2 {

namespace {
constexpr char kSyncroSig[] = "SyncrosoftContentHeader";
constexpr int kGuardFrames = 4;

float bitsToFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
}  // namespace

std::unique_ptr<SampleBank> SampleBank::open(const std::filesystem::path& path) {
    auto sb = std::make_unique<SampleBank>();
    sb->bank_ = MilanBank::open(path);
    auto readZones = [&](const BankSlot& slot) {
        std::vector<Zone> zones;
        if (slot.size == 0) return zones;
        auto z = sb->bank_->read(slot);
        size_t skip = 0;
        if (z.size() > 31 && std::memcmp(z.data(), kSyncroSig, sizeof(kSyncroSig) - 1) == 0)
            skip = 8 + rd32(z.data() + 27);
        for (size_t o = skip; o + 0x40 <= z.size(); o += 0x40) {
            const uint8_t* r = z.data() + o;
            Zone zn;
            zn.velLo = r[0];
            zn.keyLo = r[1];
            zn.velHi = r[2];
            zn.keyHi = r[3];
            zn.rootFlags = rd32(r + 4);
            zn.sample = rd32(r + 8);
            zn.frames = rd32(r + 0x10);
            zn.loopLen = rd32(r + 0x14);
            zn.tune = rdf(r + 0x18);
            zn.loopTune = rdf(r + 0x1c);
            zn.pan = rdf(r + 0x20);
            zn.volume = rdf(r + 0x24);
            // FIX: records without a valid sample stay in the table: the lookup can land on them (= no voice)
            zones.push_back(zn);
        }
        return zones;
    };
    for (const auto& e : sb->bank_->entries) {
        Instrument ins;
        ins.name = e.name;
        ins.zones = readZones(e.slot1);
        ins.ecoZones = readZones(e.slot2);
        sb->instruments.push_back(std::move(ins));
    }
    return sb;
}

const SampleData* SampleBank::sample(uint32_t index) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(index);
        if (it != cache_.end()) return it->second.get();
    }
    std::unique_ptr<SampleData> sd;
    if (index < bank_->records.size()) {
        auto blk = bank_->read(bank_->records[index]);
        sd = std::make_unique<SampleData>();
        if (!decodeSampleBlock(blk.data(), blk.size(), *sd)) sd.reset();
    }
    std::lock_guard<std::mutex> lk(mu_);
    return cache_.emplace(index, std::move(sd)).first->second.get();
}

// Sample block: 16-byte header | [compressed: seek table, 256*channels bytes] | audio
//   [4..12) PCM: frames (incl. 2 pad), rate; compressed: a, b with rate = 2a + b
//   [12] channels  [13] format (1 compressed, 2/4 int16 PCM)  [14],[15] compressed: initial exponent L/R
bool decodeSampleBlock(const uint8_t* b, size_t size, SampleData& out) {
    if (size < 16) return false;
    const int ch = b[12], fmt = b[13];
    if (ch < 1 || ch > 2) return false;
    out.channels = ch;
    out.format = fmt;
    if (fmt == 1) {
        out.rate = int(2 * int32_t(rd32(b + 4)) + int32_t(rd32(b + 8)));
        const size_t hdr = 16 + 256 * size_t(ch);
        if (size < hdr) return false;
        const size_t nfr = (size - hdr) / ch;
        out.frames = uint32_t(nfr);
        out.data.assign((nfr + kGuardFrames) * ch, 0.f);
        out.delta.assign((nfr + kGuardFrames) * ch, 0.f);
        // FIX: one word more than the table: a mono seek to checkpoint 63 also loads +0x34 from the word after it
        // (the first 4 audio bytes), see Voice::startOsc
        out.checkpoint.assign(64 * size_t(ch) + 1, 0.f);
        for (int i = 0; i <= 64 * ch && 16 + 4 * size_t(i) + 4 <= size; ++i)  // (w & 0xffffff) - 0x200000) * 2^-21
            out.checkpoint[i] = float(double(int32_t(rd32(b + 16 + 4 * i) & 0xffffffu) - 0x200000) *
                                      double(4.7683716e-07f));
        for (int c = 0; c < ch; ++c) {
            int k = b[14 + c];
            float y = 0.f;
            for (size_t n = 0; n < nfr; ++n) {
                const int code = b[hdr + n * ch + c];
                int32_t val;
                if (code & 0x80) {
                    ++k;
                    val = int32_t(uint32_t(code - 0xC0) << k);
                } else {
                    val = int32_t(uint32_t(code - 0x40) << k);
                    if (((code + 0x20) & 0x40) && k) --k;
                }
                // Hypersonic: float_from_bits(0x40C00000 + val) - 6.0  (== val * 2^-21 for |val| < 2^22)
                const float d = bitsToFloat(0x40C00000u + uint32_t(val)) - 6.0f;
                out.delta[n * ch + c] = d;
                y += d;
                out.data[n * ch + c] = y;  // FIX: value after this frame's delta (the voice adds it on load)
            }
        }
        return true;
    }
    if (fmt == 2 || fmt == 4) {
        const uint32_t hdrFrames = rd32(b + 4);
        out.rate = int(rd32(b + 8));
        size_t avail = (size - 16) / (2 * size_t(ch));
        size_t nfr = std::min<size_t>(hdrFrames, avail);
        out.frames = uint32_t(nfr);
        out.data.assign((nfr + kGuardFrames) * ch, 0.f);
        const uint8_t* p = b + 16;
        for (size_t i = 0; i < nfr * ch; ++i) out.data[i] = float(int16_t(rd16(p + 2 * i))) / 32768.f;
        return true;
    }
    return false;
}

}  // namespace hs2
