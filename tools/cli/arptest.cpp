// arptest: runs an arpeggiator scenario on the C++ port (hs2::Arp) and prints the same log as
// tools/emu/arp_oracle.py prints for the original. Compare with tools/emu/arp_compare.py.
//
// Usage: arptest <scenario.txt> [--content "C:/Hypersonic 2 Content"] [--full]
//
// The patch is loaded as Engine::loadPatch (FUN_1007bbb0) does it for the arp: new part (Arp::init), part
// setSampleRate (Arp::setSampleRate), part host sync with the synth's stored tempo (Arp::sync(0, 120, 0)), then the
// patch load FUN_10016f70: every patch param 0..0xff whose stored value is within 0..1 goes through the part setter
// FUN_100121a0 (emulated below: clamp, store, route 0x11..0x21 to Arp::setParam, 0x1b to the phrase load, 0xac..0xfb
// to Arp::setStep). Scenario ops: see arp_oracle.py.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "hs2/arp.h"
#include "hs2/bank.h"
#include "hs2/dsp.h"
#include "hs2/patch.h"

namespace fs = std::filesystem;

namespace {

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

double dbl(const std::string& h) {
    const uint64_t u = std::strtoull(h.c_str(), nullptr, 16);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

fs::path findBank(const fs::path& dir, int number) {
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        const std::string n = de.path().filename().string();
        if (n.size() > 4 && n[3] == ' ' && std::isdigit((unsigned char)n[0]) && std::stoi(n.substr(0, 3)) == number &&
            de.is_regular_file())
            return de.path();
    }
    return {};
}

// The part's patch-param words, its phrase slot 0 and the arp routing of its setter FUN_100121a0 (elem 0).
struct PartSetter {
    hs2::Arp& arp;
    fs::path content;
    float p[0x100] = {};
    std::vector<uint8_t> slot0;  // part+0xc48[0]

    // FUN_10016e20(0): slot 0 becomes current; its phrase goes to the arp (FUN_10045510(0, obj + 0x1f0))
    void selectSlot0() {
        if (!slot0.empty()) arp.setPhraseObject(slot0);
    }
    void set(int idx, float v) {
        // clamp (@100121a8): outside 0..1 -> 1 if > 0.5 else 0
        float w = v;
        if (v < 0.f || v > 1.f) w = v > 0.5f ? 1.f : 0.f;
        if (idx >= 0xfc) return;
        p[idx] = w;
        if (idx >= 0x11 && idx <= 0x21 && idx != 0x1a && idx != 0x1b) {
            arp.setParam(idx - 0x11, w);
        } else if (idx == 0x1b) {  // @10013445: load into slot 0, FUN_10016e20(0)
            const int bank = int(double(p[0x1a]) * double(128.9f));
            const int prog = int(double(p[0x1b]) * double(128.9f));
            if (bank + prog == 0) return;
            auto obj = hs2::Arp::loadPhraseEntry(content, bank, prog);
            if (obj.empty()) return;
            slot0 = std::move(obj);
            selectSlot0();
        } else if (idx == 0x95) {  // @1001337f: FUN_10016e20(trunc(v * 64.9)); only slot 0 exists here
            if (int(double(w) * double(64.9f)) == 0) selectSlot0();
        } else if (idx >= 0xac && idx <= 0xfb) {
            arp.setStep(idx, w);
        }
    }
};

// part+0x4d0 of a new part (part ctor FUN_10018b80), arp-related words; everything else 0
float partDefault(int idx) {
    if (idx == 0x17) return 0.8f;
    if (idx == 0x18 || idx == 0x1c) return 0.5f;
    if (idx == 0x1d) {
        const uint32_t u = 0x3ed47ae1;
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    if (idx == 0x1e) return 1.f;
    if (idx >= 0xac && idx <= 0xfb) return 0.5f;
    return 0.f;
}

std::string hexState(const hs2::Arp& a) {
    uint8_t b[sizeof(hs2::ArpState)];
    a.stateBytes(b);
    std::string s;
    char buf[3];
    for (uint8_t x : b) {
        std::snprintf(buf, sizeof buf, "%02x", x);
        s += buf;
    }
    return s;
}

uint32_t crcState(const hs2::Arp& a) {
    uint8_t b[sizeof(hs2::ArpState)];
    a.stateBytes(b);
    return crc32(b, sizeof b);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: arptest <scenario.txt> [--content dir] [--full]\n");
        return 2;
    }
    fs::path content = "C:/Hypersonic 2 Content";
    bool full = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--content") && i + 1 < argc) content = argv[++i];
        else if (!std::strcmp(argv[i], "--full")) full = true;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    int bank = -1, prog = -1;
    float sr = 44100.f;
    for (const auto& l : lines) {
        std::istringstream ss(l);
        std::string op;
        ss >> op;
        if (op == "patch") ss >> bank >> prog;
        else if (op == "sr") ss >> sr;
        else if (op == "content") {
            std::string rest;
            std::getline(ss, rest);
            content = rest.substr(rest.find_first_not_of(' '));
        }
    }

    hs2::MsvcRand rnd;
    hs2::Arp arp(rnd);
    PartSetter setter{arp, content, {}, {}};

    // ---- Engine::loadPatch
    const fs::path pb = findBank(content / "Patches", bank);
    if (pb.empty()) {
        std::fprintf(stderr, "no patch bank %d\n", bank);
        return 1;
    }
    auto mb = hs2::MilanBank::open(pb);
    auto data = mb->read(mb->entries.at(size_t(prog)).slot1);
    while (data.size() >= 31 && std::memcmp(data.data(), "SyncrosoftContentHeader", 23) == 0)
        data = data.subspan(8 + hs2::rd32(data.data() + 27));
    const hs2::Patch patch = hs2::Patch::parse(data);
    arp.init();  // part ctor FUN_10018b80
    // part setSampleRate FUN_100137f0: every patch param through the setter with the part's current values, then
    // the arp's setSampleRate
    for (int i = 0; i < 0xfc; ++i) setter.set(i, partDefault(i));
    arp.setSampleRate(sr);
    arp.sync(0, 120.0, 0.0);  // part sync FUN_1000ef10 with the synth's stored tempo (Engine::loadPatch)
    for (int i = 0; i < 0x100; ++i) {  // patch load FUN_10016f70
        float v = patch.params[size_t(i)];
        if (v < 0.f || !(v <= 1.f)) continue;  // "Warning - Patch parameter" (not applied)
        if (i == 0x18 && int32_t(patch.version) < 0xbec874c) v = 0.5f;
        setter.set(i, v);
    }
    const auto tr = hs2::Arp::patchTrailer(data);
    if (tr.kind == 0) {  // @100174f1 legacy patch
        setter.set(0x95, 0.f);
        setter.set(0x96, 0.f);
        setter.set(0x97, 1.f);
        setter.set(0x1e, 1.f);
    } else if (tr.kind == 2) {  // @100176cc embedded phrase slots
        for (size_t i = 0; i < tr.slots.size(); ++i)
            if (i == 0 && !tr.slots[0].empty()) {
                setter.slot0.assign(tr.slots[0].begin(), tr.slots[0].end());
                setter.selectSlot0();  // slot == part+0xd48 (0)
            }
        setter.set(0x95, setter.p[0x95]);
    }
    rnd.state = 1;
    std::printf("load %d/%d %08x%s%s\n", bank, prog, crcState(arp), full ? " " : "", full ? hexState(arp).c_str() : "");

    // rand() calls since the load: step a copy of the generator from 1 until it matches.
    auto countCalls = [&](uint32_t from, uint32_t to) {
        int n = 0;
        hs2::MsvcRand r;
        r.state = from;
        while (r.state != to && n < 100000000) {
            r.next();
            ++n;
        }
        return n;
    };
    const uint32_t rand0 = 1;
    int chunk = 0;
    for (const auto& l : lines) {
        std::istringstream ss(l);
        std::string op;
        ss >> op;
        if (op.empty() || op[0] == '#') continue;
        if (op == "rand") {
            std::string h;
            ss >> h;
            rnd.state = uint32_t(std::strtoul(h.c_str(), nullptr, 0));
        } else if (op == "P") {
            std::string a, b;
            ss >> a >> b;
            const int idx = int(std::strtol(a.c_str(), nullptr, 16));
            const uint32_t bits = uint32_t(std::strtoul(b.c_str(), nullptr, 16));
            float v;
            std::memcpy(&v, &bits, 4);
            setter.set(idx, v);
            std::printf("P %#x %08x\n", idx, bits);
        } else if (op == "S") {
            std::string f, b, q;
            ss >> f >> b >> q;
            const int flags = int(std::strtol(f.c_str(), nullptr, 16));
            arp.sync(flags, dbl(b), dbl(q));
            std::printf("S %#x\n", flags);
        } else if (op == "M") {
            std::string a, b, c;
            ss >> a >> b >> c;
            uint8_t m[4] = {uint8_t(std::strtoul(a.c_str(), nullptr, 16)), uint8_t(std::strtoul(b.c_str(), nullptr, 16)),
                            uint8_t(std::strtoul(c.c_str(), nullptr, 16)), 0};
            const bool r = arp.midiIn(m);
            std::printf("M %02x %02x %02x -> %d", m[0], m[1], m[2], int(r));
            if (!r && m[0] == 0xb0 && m[1] == 0x40 && m[2] < 0x40) {  // part FUN_10017880 case 0x40 recursion
                const uint8_t off[4] = {0x80, 0xff, 0, 0};
                std::printf(" | 80 ff 00 -> %d", int(arp.midiIn(off)));
            }
            std::printf("\n");
        } else if (op == "R") {
            std::string a, b, c;
            ss >> a >> b >> c;
            int n = std::atoi(a.c_str());
            double beat = dbl(b);
            const double bps = dbl(c);
            uint8_t ev[4] = {0, 0, 0, 0};  // FUN_100191b0: one event per call, zeroed once
            int off = 0;
            std::string evs;
            int r = arp.next(n, beat, ev);
            while (r < n) {
                if (r > 0) {
                    off += r;
                    n -= r;
                    beat = double(r) * bps + beat;
                }
                if (ev[0]) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%s%d:%02x%02x%02x%02x", evs.empty() ? "" : " ", off, ev[0], ev[1],
                                  ev[2], ev[3]);
                    evs += buf;
                }
                r = arp.next(n, beat, ev);
            }
            std::printf("R%d %s | %08x%s%s\n", chunk, evs.c_str(), crcState(arp), full ? " " : "",
                        full ? hexState(arp).c_str() : "");
            ++chunk;
        } else if (op == "D") {
            std::printf("D %s\n", hexState(arp).c_str());
        }
    }
    std::printf("end rand=%#010x calls=%d\n", rnd.state, countCalls(rand0, rnd.state));
    return 0;
}
