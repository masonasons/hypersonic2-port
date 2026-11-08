#include "hs2/combi.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "hs2/bank.h"

namespace hs2 {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
std::string cstr(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}
}  // namespace

Combi Combi::parse(std::span<const uint8_t> data) {
    const uint8_t* d = data.data();
    size_t n = data.size();
    // VST 2 bank (.fxb): 'CcnK', size, 'FBCh', ..., chunk size (big-endian) at 0x9c, chunk at 0xa0
    if (n >= 0xa0 && std::memcmp(d, "CcnK", 4) == 0 && std::memcmp(d + 8, "FBCh", 4) == 0) {
        const uint32_t size = be32(d + 0x9c);
        if (size > n - 0xa0) throw std::runtime_error("truncated .fxb chunk");
        d += 0xa0;
        n = size;
    }
    // setChunk: 1000 .. 2000000 bytes, 'StSS', a bank chunk ('B', 1) of version 0x990EDF .. 0xD1CEF00
    if (n < 0x58 || std::memcmp(d, "StSS", 4) != 0) throw std::runtime_error("not an StSS chunk");
    if (d[0xc] != 'B' || d[0xd] != 1) throw std::runtime_error("not a combi (bank) chunk");
    Combi c;
    c.version = rd32(d + 4);
    if (c.version < 0x990EDF || c.version >= 0xD1CEF01) throw std::runtime_error("unsupported combi version");
    uint32_t off[18];
    for (int i = 0; i < 18; ++i) off[i] = rd32(d + 0x10 + 4 * i);
    auto end = [&](uint32_t o) {  // a part blob runs to the next stored block (FUN_10016f70 reads it in place)
        size_t e = n;
        for (uint32_t x : off)
            if (x > o && x < e) e = x;
        return e;
    };
    for (int k = 0; k < 16; ++k) {
        const uint32_t o = off[k];
        if (o == 0) continue;
        if (o >= n) throw std::runtime_error("bad combi part offset");
        std::vector<uint8_t> blob;
        if (o >= 0x10 && d[o + 3] == 'S') {  // already has its header (setChunk tests the byte at +3)
            blob.assign(d + o, d + end(o));
        } else {  // setChunk rebuilds the 16-byte header: 'StSS', the chunk's version, 0, 'P', 1, 0, 0
            const uint8_t hdr[16] = {'S', 't', 'S', 'S', d[4], d[5], d[6], d[7], 0, 0, 0, 0, 'P', 1, 0, 0};
            blob.assign(hdr, hdr + 16);
            blob.insert(blob.end(), d + o, d + end(o));
        }
        c.parts[size_t(k)] = Patch::parse(blob);
    }
    if (off[16] + 4 * 0x420 > n || off[17] + 0x18 > n) throw std::runtime_error("truncated combi");
    for (int i = 0; i < 4; ++i) {
        const uint8_t* r = d + off[16] + 0x420 * i;
        c.globalFx[size_t(i)].name = cstr(r, 0x20);
        for (int j = 0; j < 256; ++j) c.globalFx[size_t(i)].params[size_t(j)] = rdf(r + 0x20 + 4 * j);
    }
    const uint8_t* s = d + off[17];
    const size_t left = n - off[17];
    c.settingsVersion = rd32(s);
    c.masterVolume = rdf(s + 4);
    for (int k = 0; k < 16; ++k) c.links[size_t(k)] = s[8 + k];
    size_t p = 0x18;
    if (c.settingsVersion > 0x33 && p + 0x20 <= left) {
        c.name = cstr(s + p, 0x20);
        p += 0x20;
    }
    if (c.settingsVersion > 0x3f && p + 0xc <= left) {
        c.hasSetup = true;
        c.velSens = rdf(s + p);
        c.touchSens = rdf(s + p + 4);
        c.memory = rdf(s + p + 8);
        if (c.settingsVersion > 0x4f && p + 0x1c <= left) {
            c.hasTune = true;
            c.tuneSemis = int32_t(rd32(s + p + 0xc));
            c.tuneFine = rdf(s + p + 0x10);
            c.hype = rdf(s + p + 0x14);
            c.gm = rd32(s + p + 0x18);
            if (c.settingsVersion > 0x5f && p + 0x1c + 0x200 <= left)
                for (int i = 0; i < 128; ++i) c.chain.push_back(int32_t(rd32(s + p + 0x1c + 4 * size_t(i))));
        }
    }
    return c;
}

Combi Combi::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + file.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(bytes);
}

std::vector<int32_t> parseCombiChain(const std::string& text) {
    std::vector<int32_t> out;
    size_t p = 0;
    bool first = true;
    while (p < text.size() && out.size() < 128) {
        size_t q = text.find('\n', p);
        if (q == std::string::npos) q = text.size();
        std::string line = text.substr(p, q - p);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (first) {
            if (line != "HS2//COMBICHAIN") throw std::runtime_error("not a combi chain");
            first = false;
        } else if (!line.empty()) {
            out.push_back(int32_t(std::strtol(line.c_str(), nullptr, 10)));
        }
        p = q + 1;
    }
    if (first) throw std::runtime_error("not a combi chain");
    return out;
}

std::filesystem::path combiPath(const std::filesystem::path& root, int bank, int index) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path patches = root / "Patches";
    for (const auto& e : fs::directory_iterator(patches, ec)) {
        const std::string n = e.path().filename().string();
        if (!e.is_directory() || n.size() < 4 || std::atoi(n.substr(0, 3).c_str()) != bank || n[3] != ' ') continue;
        for (const auto& f : fs::directory_iterator(e.path(), ec)) {
            const std::string m = f.path().filename().string();
            if (m.size() > 4 && std::isdigit(static_cast<unsigned char>(m[0])) && std::atoi(m.c_str()) == index &&
                f.path().extension() == ".fxb")
                return f.path();
        }
    }
    return {};
}

}  // namespace hs2
