#include "hs2/patch.h"

#include <cstring>
#include <stdexcept>

#include "hs2/bank.h"

namespace hs2 {

namespace {
std::string cstr(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}
void readParams(const uint8_t* p, ParamArray& out) {
    for (int i = 0; i < 256; ++i) out[i] = rdf(p + 4 * i);
}
}  // namespace

Patch Patch::parse(std::span<const uint8_t> blob) {
    const uint8_t* d = blob.data();
    size_t n = blob.size();
    if (n >= 0x40 && std::memcmp(d, "StSS", 4) != 0 && std::memcmp(d + 0x3c, "StSS", 4) == 0) {
        d += 0x3c;  // inside a VST fxp/fxb chunk
        n -= 0x3c;
    }
    if (n < 0x620 || std::memcmp(d, "StSS", 4) != 0) throw std::runtime_error("not a StSS patch");
    Patch p;
    p.blob.assign(d, d + n);
    p.version = rd32(d + 4);
    const uint32_t count = rd32(d + 0x14);
    for (int i = 0; i < 4; ++i) p.sendFxFlags[i] = d[0x18 + i];
    p.name = cstr(d + 0x20, 0x20);
    p.shortName = cstr(d + 0x40, 0x40);
    p.description = cstr(d + 0x80, 0x80);
    p.controllers = cstr(d + 0x100, 0x80);
    p.author = cstr(d + 0x180, 0x40);
    for (int i = 0; i < 6; ++i) p.quickControls[i] = cstr(d + 0x1c0 + 0x10 * i, 0x10);
    readParams(d + 0x220, p.params);

    int nfx = 0;
    for (auto f : p.sendFxFlags) nfx += f != 0;
    size_t o = 0x620;
    for (uint32_t i = 0; i < count + uint32_t(nfx); ++i, o += 0x420) {
        if (o + 0x420 > n) break;
        PatchRecord r;
        r.name = cstr(d + o, 0x20);
        readParams(d + o + 0x20, r.params);
        (i < count ? p.elements : p.sendFx).push_back(std::move(r));
    }
    if (o + 4 <= n) {
        p.tailMarker = rd32(d + o);
        if (p.tailMarker == 2 && o + 4 + 256 <= n)
            for (int i = 0; i < 128; ++i) p.ccMap.push_back(int16_t(rd16(d + o + 4 + 2 * i)));
    }
    // FIX: patch load FUN_10016f70 @10017250: element param 0x7d (Note-on Click) of patches older than 0xA87569 is
    // scaled by 0.3, or cleared when > 0.99.
    if (int32_t(p.version) < 0xA87569)
        for (auto& r : p.elements) {
            float& v = r.params[0x7d];
            v = v > 0.99f ? 0.f : float(double(v) * double(0.3f));
        }
    return p;
}

}  // namespace hs2
