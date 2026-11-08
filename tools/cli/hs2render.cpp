// hs2render: render one Hypersonic 2 patch with the port's engine, using the same test sequence as
// tools/reaper/hs2_render.lua so results line up with reference renders (B###_P###.wav).
//   hs2render --bank 7 --patch 0 [--content "C:\Hypersonic 2 Content"] [--sr 44100] [--lead 4] [--out file.wav]
// Debug options for comparisons with the emulated original (tools/emu/part_oracle.py):
//   --dry               patch send levels (0x3f..0x42) set to 0 and the master tone enhancer undone, so the output is
//                       the part's main out (dry bus * patch gain * master), like part_oracle's default output
//   --notes "t:len:pitch:vel,..."   replace the test sequence (seconds after the lead-in)   --tail S (default 3)
//   --events "t:kind:a[:b],..."     extra MIDI at t seconds after the lead-in (sorted after the notes at equal times):
//                       cc:num:value, pb:value14 (0..16383, 8192 = centre), at:value, on:note:vel, off:note,
//                       raw:status:d1:d2[:d3] (decimal bytes), par:elem:idx:value (PatchPlayer::setParam between
//                       blocks, elem 0 = patch, idx may be hex; as part_oracle --events par)
//   --host "t:bpm:play[:ppq],..."   host transport (plugin-level sync, as part_oracle --host): from t seconds after the
//                       lead-in (t < 0: from the start) the host runs at bpm, playing (1) or stopped (0); ppq jumps
//                       the song position there. Starts stopped at 120 bpm, ppq 0; 4/4. The player gets resume()
//                       before the first block and the transport every block. Without --host the player is synced
//                       once after loading (120 bpm, flags 0), as part_oracle --tempo 120 does, and gets no transport.
//   --solo N[,M...]     mute every element record except these (0-based, inserts included)
//   --block N           host block size (default 32): blocks aligned to absolute time, events inside them
// The player is called with PatchPlayer::processBlock per host block, which splits at the events exactly as the
// original's main render does (as part_oracle.py drives the original): the part's LFO ticks, sleep and bias depend
// on the call sizes.
// Engine mode (hs2::Engine, the 16-part instrument with global send FX; compare with tools/emu/combi_oracle.py):
//   --combi FILE.fxb    load a combi (setChunk)          --part "ch:bank:patch,..."  load patches into parts 1..16
//   --engine            with --bank/--patch: the patch in part 1 of an Engine instead of a lone PatchPlayer
//   --outputs N         stereo outputs (default 1; only output 1 is written)
//   --chain FILE.h2c    print a Combi Chain and exit
//   --memory M          memory mode before loading (0 ECO, 0.5 MID default, 1 XXL); --vel-sens / --touch-sens / --hype V:
//                       the setup values after loading (as combi_oracle.py)
//   notes take an optional 5th field, the MIDI channel 1..16 ("t:len:pitch:vel:ch"); events an optional channel
//   after the kind ("t:cc/3:7:100"); raw events carry their own; pc:program = program change (recorded only);
//   control events between blocks: load:ch:bank:patch (Engine::loadPatch), vst:index:value (Engine::setParameter),
//   combi:INDEX (Engine::loadCombi with factory combi 120/INDEX).
//   Without --host the engine is synced once after loading (FUN_1007baf0, 120 bpm, flags 0).
//   --load-bank         loads (--part, --bank, load:) pass the patch's bank to Engine::loadPatch (the default scene root
//                       rule), as the plugin's own loads do; default -1 (as combi_oracle.py)
//   --skip N            render N frames before the notes / events / host changes (all shifted by N) and leave them out of
//                       the output: the plugin test host's layout (a patch-change block, then the test)
//   --ring-out          effect tails ring out (Engine/PatchPlayer::setRingOut; NOT the original, as the plugin's default)
//   --cut-on-load       engine mode: a load fades out the part's previous patch (Engine::setCutOnLoad; NOT the
//                       original, as the plugin's default)
//   an event time "@F" is an absolute frame (not shifted by --skip), e.g. "@32:load:1:15:23"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "hs2/combi.h"
#include "hs2/content.h"
#include "hs2/engine.h"
#include "hs2/patch.h"
#include "hs2/synth.h"

namespace {

struct Note {
    double start, length;
    int pitch, velocity;
    int channel = 1;
};
// Must match SEQ in tools/reaper/hs2_render.lua.
const Note kSeq[] = {
    {0.0, 2.0, 60, 100}, {3.5, 1.0, 36, 100}, {5.5, 1.0, 84, 100}, {7.5, 1.0, 60, 30},
    {9.5, 1.0, 60, 127}, {11.5, 1.5, 60, 100}, {11.5, 1.5, 64, 100}, {11.5, 1.5, 67, 100},
};
constexpr double kSeqEnd = 13.0;

struct HostChange {
    double t, bpm;
    bool play;
    bool jump;
    double ppq;
};

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        size_t q = s.find(c, p);
        out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

// --dry: undo PatchPlayer's master tone enhancer (synth.cpp; y = (x + carry) * g + x, carry = lp2(x) - x),
// which is exactly invertible up to float rounding. Debug only: the constants mirror synth.cpp.
void undoEnhancer(std::vector<float>& lr, int sr) {
    const float g = 0.33f * 2.2f, a = 700.f / float(sr);
    const int mode = sr >= 64000 ? 2 : 1;
    for (int ch = 0; ch < 2; ++ch) {
        float s1 = 0, s2 = 0, carry[2] = {0, 0};
        int phase = 0;
        for (size_t n = 0; n < lr.size() / 2; ++n) {
            float& y = lr[2 * n + ch];
            float& c = carry[mode == 2 ? phase : 0];
            const float in = (y - c * g) / (1.f + g);
            y = in;
            s1 += (in - s1) * a;
            s2 += (s1 - s2) * a;
            c = s2 - in;
            phase ^= 1;
        }
    }
}

void writeWavFloat(const std::string& path, const std::vector<float>& lr, int sr) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(lr.size() * 4);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(2); u32(sr); u32(sr * 8); u16(8); u16(32);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(lr.data()), bytes);
}

struct EngineArgs {
    std::string content, combi, parts, eventsArg, hostArg, out;
    int bank = -1, prog = 0, sr = 44100, block = 32, outputs = 1, skip = 0;
    bool loadBank = false, ringOut = false, cutOnLoad = false;
    double lead = 4.0, tail = 3.0, seqEnd = kSeqEnd;
    float memory = 0.5f, velSens = -1.f, touchSens = -1.f, hype = -1.f;  // < 0: unchanged
    std::vector<Note> seq;
};

hs2::Patch loadBankPatch(hs2::Content& c, int bank, int prog) {
    auto path = c.patchBankPath(bank);
    if (path.empty()) throw std::runtime_error("no patch bank " + std::to_string(bank));
    auto pb = hs2::MilanBank::open(path);
    if (prog < 0 || prog >= int(pb->entries.size())) throw std::runtime_error("no such patch");
    return hs2::Patch::parse(pb->read(pb->entries[size_t(prog)].slot1));
}

// Engine mode: the same test sequence / events / host model as the single-part path, with MIDI channels.
int runEngine(const EngineArgs& a) {
    hs2::Content c(a.content);
    hs2::Engine eng(c, float(a.sr), a.outputs);
    if (a.memory != 0.5f) eng.setMemoryMode(a.memory);
    eng.setRingOut(a.ringOut);
    eng.setCutOnLoad(a.cutOnLoad);
    if (!a.combi.empty()) {
        const auto cb = hs2::Combi::load(a.combi);
        std::printf("combi '%s' (version %#x, settings %#x)\n", cb.name.c_str(), cb.version, cb.settingsVersion);
        for (int k = 0; k < 16; ++k)
            if (cb.parts[size_t(k)]) std::printf("  part %2d: %s\n", k + 1, cb.parts[size_t(k)]->name.c_str());
        eng.loadCombi(cb);
    }
    if (!a.parts.empty())
        for (const auto& item : split(a.parts, ',')) {
            int ch = 0, b = 0, p = 0;
            if (std::sscanf(item.c_str(), "%d:%d:%d", &ch, &b, &p) != 3 || ch < 1 || ch > 16)
                throw std::runtime_error("bad --part item " + item);
            eng.loadPatch(ch - 1, loadBankPatch(c, b, p), a.loadBank ? b : -1);
            std::printf("  part %2d: %s\n", ch, eng.part(ch - 1).patch().name.c_str());
        }
    if (a.bank >= 0 && a.parts.empty()) eng.loadPatch(0, loadBankPatch(c, a.bank, a.prog), a.loadBank ? a.bank : -1);
    if (a.velSens >= 0.f) eng.setVelSens(a.velSens);
    if (a.touchSens >= 0.f) eng.setTouchSens(a.touchSens);
    if (a.hype >= 0.f) eng.setHype(a.hype);

    // ctl: 0 MIDI, 1 load:ch:bank:patch, 2 vst:index:value, 3 combi:index (control events between blocks, before MIDI
    // at equal times)
    struct Ev { int64_t t; uint8_t m[4]; int ctl = 0, i1 = 0, i2 = 0, i3 = 0; float v = 0; };
    std::vector<Ev> evs;
    for (auto& n : a.seq) {
        const uint8_t ch = uint8_t((n.channel - 1) & 15);
        evs.push_back({int64_t((a.lead + n.start) * a.sr), {uint8_t(0x90 | ch), uint8_t(n.pitch), uint8_t(n.velocity), 0}});
        evs.push_back({int64_t((a.lead + n.start + n.length) * a.sr), {uint8_t(0x80 | ch), uint8_t(n.pitch), 0, 0}});
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.t < y.t; });
    double end = a.lead + a.seqEnd;
    if (!a.eventsArg.empty()) {
        std::vector<Ev> extra;
        for (const auto& item : split(a.eventsArg, ',')) {
            const auto f = split(item, ':');
            auto num = [&](size_t i) { return i < f.size() ? std::atoi(f[i].c_str()) : 0; };
            if (f.size() < 3) throw std::runtime_error("bad --events item " + item);
            const bool absolute = !f[0].empty() && f[0][0] == '@';  // "@F": frame F of the output with --skip
            const double t = absolute ? double(std::atoll(f[0].c_str() + 1) - a.skip) / a.sr - a.lead
                                      : std::atof(f[0].c_str());
            Ev e{absolute ? std::atoll(f[0].c_str() + 1) - a.skip : int64_t((a.lead + t) * a.sr), {0, 0, 0, 0}};
            std::string k = f[1];
            int ch = 1;
            if (const auto sl = k.find('/'); sl != std::string::npos) {
                ch = std::atoi(k.c_str() + sl + 1);
                k = k.substr(0, sl);
            }
            const uint8_t c4 = uint8_t((ch - 1) & 15);
            if (k == "cc") e.m[0] = 0xb0 | c4, e.m[1] = uint8_t(num(2)), e.m[2] = uint8_t(num(3));
            else if (k == "pb") e.m[0] = 0xe0 | c4, e.m[1] = uint8_t(num(2) & 127), e.m[2] = uint8_t((num(2) >> 7) & 127);
            else if (k == "at") e.m[0] = 0xd0 | c4, e.m[1] = uint8_t(num(2));
            else if (k == "on") e.m[0] = 0x90 | c4, e.m[1] = uint8_t(num(2)), e.m[2] = uint8_t(num(3));
            else if (k == "off") e.m[0] = 0x80 | c4, e.m[1] = uint8_t(num(2));
            else if (k == "pc") e.m[0] = 0xc0 | c4, e.m[1] = uint8_t(num(2));
            else if (k == "raw")
                for (int b = 0; b < 4; ++b) e.m[b] = uint8_t(num(2 + size_t(b)));
            else if (k == "load" && f.size() >= 5) e.ctl = 1, e.i1 = num(2), e.i2 = num(3), e.i3 = num(4);
            else if (k == "combi") e.ctl = 3, e.i1 = num(2);
            else if (k == "vst" && f.size() >= 4)
                e.ctl = 2, e.i1 = int(std::strtol(f[2].c_str(), nullptr, 0)), e.v = float(std::atof(f[3].c_str()));
            else throw std::runtime_error("bad --events kind " + k + " (engine mode)");
            extra.push_back(e);
            end = std::max(end, a.lead + t);
        }
        std::stable_sort(extra.begin(), extra.end(), [](const Ev& x, const Ev& y) { return x.t < y.t; });
        std::vector<Ev> merged;
        std::merge(evs.begin(), evs.end(), extra.begin(), extra.end(), std::back_inserter(merged),
                   [](const Ev& x, const Ev& y) { return x.t < y.t; });
        // control events go first at equal times (as combi_oracle.py)
        std::stable_sort(merged.begin(), merged.end(),
                         [](const Ev& x, const Ev& y) { return x.t < y.t || (x.t == y.t && x.ctl && !y.ctl); });
        evs.swap(merged);
    }
    std::vector<HostChange> host;
    for (const auto& item : a.hostArg.empty() ? std::vector<std::string>{} : split(a.hostArg, ',')) {
        const auto f = split(item, ':');
        if (f.size() < 3) throw std::runtime_error("bad --host item " + item);
        host.push_back({std::atof(f[0].c_str()), std::atof(f[1].c_str()), std::atoi(f[2].c_str()) != 0,
                        f.size() > 3, f.size() > 3 ? std::atof(f[3].c_str()) : 0.0});
    }
    const bool hostMode = !a.hostArg.empty();
    if (hostMode) eng.resume();
    else eng.syncTempo(120.0, 120.0 / 60.0 / a.sr, 4.0, 0.0, 0.0, 0);  // as combi_oracle without --host

    const int sr = a.sr, block = a.block;
    for (auto& e : evs) e.t += a.skip;  // --skip: the whole test starts N frames later
    const int64_t total = int64_t((end + a.tail) * sr) + a.skip;
    std::vector<float> lr(size_t(total) * 2, 0.f);
    double hBpm = 120.0, hPpq = 0.0;
    bool hPlay = false;
    int64_t hT = 0;
    size_t hi = 0, ei = 0;
    std::vector<hs2::MidiEvent> blockEvs;
    for (int64_t t = 0; t < total;) {
        int64_t n = std::min<int64_t>(block - t % block, total - t);
        if (ei < evs.size() && evs[ei].ctl && evs[ei].t <= t) {  // a control event between blocks
            const Ev& e = evs[ei++];
            if (e.ctl == 1) {
                eng.loadPatch(e.i1 - 1, loadBankPatch(c, e.i2, e.i3), a.loadBank ? e.i2 : -1);
            } else if (e.ctl == 3) {
                const auto path = hs2::combiPath(c.root(), 120, e.i1);
                if (path.empty()) throw std::runtime_error("no combi 120/" + std::to_string(e.i1));
                eng.loadCombi(hs2::Combi::load(path));
            } else {
                eng.setParameter(e.i1, e.v);
            }
            continue;
        }
        for (size_t j = ei; j < evs.size() && evs[j].t < t + n; ++j)
            if (evs[j].ctl) {
                n = evs[j].t - t;
                break;
            }
        hs2::HostTransport tr;
        if (hostMode) {
            while (hi < host.size() && int64_t(std::floor((a.lead + host[hi].t) * sr)) + a.skip <= t) {
                if (hPlay) hPpq = hPpq + double(t - hT) * (hBpm / 60.0 / sr);
                hT = t;
                hBpm = host[hi].bpm;
                hPlay = host[hi].play;
                if (host[hi].jump) hPpq = host[hi].ppq;
                ++hi;
            }
            tr.bpm = hBpm;
            tr.playing = hPlay;
            tr.ppq = hPlay ? hPpq + double(t - hT) * (hBpm / 60.0 / sr) : hPpq;
            tr.barStart = std::floor(tr.ppq / 4.0) * 4.0;
        }
        blockEvs.clear();
        while (ei < evs.size() && evs[ei].t < t + n) {
            hs2::MidiEvent e;
            e.offset = int(std::max<int64_t>(0, evs[ei].t - t));
            std::memcpy(e.data, evs[ei].m, 4);
            blockEvs.push_back(e);
            ++ei;
        }
        eng.processBlock(&lr[size_t(t) * 2], int(n), blockEvs.data(), int(blockEvs.size()), hostMode ? &tr : nullptr);
        t += n;
    }
    for (const auto& r : eng.takeProgramRequests())
        std::printf("  program change request: channel %d bank %d program %d\n", r.channel + 1, r.bank, r.program);
    if (a.skip > 0) lr.erase(lr.begin(), lr.begin() + std::min<ptrdiff_t>(ptrdiff_t(lr.size()), ptrdiff_t(a.skip) * 2));
    writeWavFloat(a.out, lr, sr);
    std::printf("wrote %s\n", a.out.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string content = "C:\\Hypersonic 2 Content", out, eventsArg, hostArg, combiArg, partsArg;
    int bank = -1, prog = 0, sr = 44100, block = 32, outputs = 1, skip = 0;
    bool loadBank = false, ringOut = false, cutOnLoad = false;
    double lead = 4.0, tail = 3.0;
    bool dry = false, engineMode = false;
    float memory = 0.5f, velSens = -1.f, touchSens = -1.f, hype = -1.f;
    std::vector<int> solo;
    std::vector<Note> seq(std::begin(kSeq), std::end(kSeq));
    double seqEnd = kSeqEnd;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (arg("--content")) content = argv[++i];
        else if (arg("--bank")) bank = std::atoi(argv[++i]);
        else if (arg("--patch")) prog = std::atoi(argv[++i]);
        else if (arg("--sr")) sr = std::atoi(argv[++i]);
        else if (arg("--lead")) lead = std::atof(argv[++i]);
        else if (arg("--out")) out = argv[++i];
        else if (arg("--tail")) tail = std::atof(argv[++i]);
        else if (arg("--block")) block = std::max(1, std::atoi(argv[++i]));
        else if (arg("--events")) eventsArg = argv[++i];
        else if (arg("--host")) hostArg = argv[++i];
        else if (std::strcmp(argv[i], "--dry") == 0) dry = true;
        else if (std::strcmp(argv[i], "--engine") == 0) engineMode = true;
        else if (std::strcmp(argv[i], "--load-bank") == 0) loadBank = true;
        else if (std::strcmp(argv[i], "--ring-out") == 0) ringOut = true;
        else if (std::strcmp(argv[i], "--cut-on-load") == 0) cutOnLoad = true;
        else if (arg("--skip")) skip = std::max(0, std::atoi(argv[++i]));
        else if (arg("--combi")) combiArg = argv[++i], engineMode = true;
        else if (arg("--part")) partsArg = argv[++i], engineMode = true;
        else if (arg("--outputs")) outputs = std::max(1, std::atoi(argv[++i]));
        else if (arg("--memory")) memory = float(std::atof(argv[++i]));
        else if (arg("--vel-sens")) velSens = float(std::atof(argv[++i]));
        else if (arg("--touch-sens")) touchSens = float(std::atof(argv[++i]));
        else if (arg("--hype")) hype = float(std::atof(argv[++i]));
        else if (arg("--chain")) {  // print a Combi Chain (.h2c) and exit
            std::ifstream f(argv[++i], std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            try {
                const auto chain = hs2::parseCombiChain(text);
                for (size_t k = 0; k < chain.size(); ++k)
                    if (chain[k] >= 0) std::printf("%3zu: bank %d combi %d\n", k, chain[k] >> 8, chain[k] & 0xff);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "error: %s\n", ex.what());
                return 1;
            }
            return 0;
        }
        else if (arg("--solo")) {
            for (const char* s = argv[++i]; *s;) {
                solo.push_back(std::atoi(s));
                while (*s && *s != ',') ++s;
                if (*s) ++s;
            }
        }
        else if (arg("--notes")) {
            seq.clear();
            seqEnd = 0;
            for (const auto& item : split(argv[++i], ',')) {
                Note n{};
                const int got = std::sscanf(item.c_str(), "%lf:%lf:%d:%d:%d", &n.start, &n.length, &n.pitch,
                                            &n.velocity, &n.channel);
                if (got < 4) {
                    std::fprintf(stderr, "bad --notes item\n");
                    return 2;
                }
                if (got == 4) n.channel = 1;
                seq.push_back(n);
                seqEnd = std::max(seqEnd, n.start + n.length);
            }
        }
        else {
            std::fprintf(stderr, "usage: hs2render --bank N --patch I [--content DIR] [--sr HZ] [--lead S] [--out F]\n");
            return 2;
        }
    }
    if (engineMode) {
        if (out.empty()) out = "engine.wav";
        if (dry) {
            std::fprintf(stderr, "--dry is not available in engine mode\n");
            return 2;
        }
        EngineArgs ea;
        ea.content = content, ea.combi = combiArg, ea.parts = partsArg, ea.eventsArg = eventsArg;
        ea.hostArg = hostArg, ea.out = out, ea.bank = bank, ea.prog = prog, ea.sr = sr, ea.block = block;
        ea.outputs = outputs, ea.lead = lead, ea.tail = tail, ea.seq = seq, ea.seqEnd = seqEnd;
        ea.skip = skip, ea.loadBank = loadBank, ea.ringOut = ringOut, ea.cutOnLoad = cutOnLoad;
        ea.memory = memory, ea.velSens = velSens, ea.touchSens = touchSens, ea.hype = hype;
        try {
            return runEngine(ea);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "error: %s\n", ex.what());
            return 1;
        }
    }
    if (bank < 0) {
        std::fprintf(stderr, "--bank is required\n");
        return 2;
    }
    if (out.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "B%03d_P%03d.wav", bank, prog);
        out = buf;
    }
    try {
        hs2::Content c(content);
        auto path = c.patchBankPath(bank);
        if (path.empty()) throw std::runtime_error("no patch bank " + std::to_string(bank));
        auto pb = hs2::MilanBank::open(path);
        if (prog >= int(pb->entries.size())) throw std::runtime_error("no such patch");
        auto patch = hs2::Patch::parse(pb->read(pb->entries[prog].slot1));
        if (dry)
            for (int k = 0; k < 4; ++k) patch.params[0x3f + k] = 0.f;  // no send FX input
        if (!solo.empty())
            for (int k = 0; k < int(patch.elements.size()); ++k)
                if (std::find(solo.begin(), solo.end(), k) == solo.end())
                    patch.elements[k].params[0] = 1.f;  // element param 0 = Mute
        std::printf("%s / %s  (%zu elements)\n", path.filename().string().c_str(), patch.name.c_str(),
                    patch.elements.size());
        hs2::PatchPlayer player(c, float(sr));
        player.setRingOut(ringOut);
        player.load(patch);
        for (auto& w : player.warnings()) std::printf("  warning: %s\n", w.c_str());

        // event list in samples: the notes (stable-sorted as before), then the extra events
        struct Ev { int64_t t; uint8_t m[4]; int par = -1, idx = 0; float value = 0; };  // par >= 0: setParam
        std::vector<Ev> evs;
        for (auto& n : seq) {
            evs.push_back({int64_t((lead + n.start) * sr), {0x90, uint8_t(n.pitch), uint8_t(n.velocity), 0}});
            evs.push_back({int64_t((lead + n.start + n.length) * sr), {0x80, uint8_t(n.pitch), 0, 0}});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
        double end = lead + seqEnd;
        if (!eventsArg.empty()) {
            std::vector<Ev> extra;
            for (const auto& item : split(eventsArg, ',')) {
                const auto f = split(item, ':');
                auto num = [&](size_t i) { return i < f.size() ? std::atoi(f[i].c_str()) : 0; };
                if (f.size() < 3) throw std::runtime_error("bad --events item " + item);
                const double t = std::atof(f[0].c_str());
                Ev e{int64_t((lead + t) * sr), {0, 0, 0, 0}};
                const std::string& k = f[1];
                if (k == "cc") e.m[0] = 0xb0, e.m[1] = uint8_t(num(2)), e.m[2] = uint8_t(num(3));
                else if (k == "pb") e.m[0] = 0xe0, e.m[1] = uint8_t(num(2) & 127), e.m[2] = uint8_t((num(2) >> 7) & 127);
                else if (k == "at") e.m[0] = 0xd0, e.m[1] = uint8_t(num(2));
                else if (k == "on") e.m[0] = 0x90, e.m[1] = uint8_t(num(2)), e.m[2] = uint8_t(num(3));
                else if (k == "off") e.m[0] = 0x80, e.m[1] = uint8_t(num(2));
                else if (k == "raw")
                    for (int b = 0; b < 4; ++b) e.m[b] = uint8_t(num(2 + b));
                else if (k == "par" && f.size() >= 5)  // par:elem:idx:value -> PatchPlayer::setParam between blocks
                    e.par = num(2), e.idx = int(std::strtol(f[3].c_str(), nullptr, 0)),
                    e.value = float(std::atof(f[4].c_str()));
                else throw std::runtime_error("bad --events kind " + k);
                extra.push_back(e);
                end = std::max(end, lead + t);
            }
            std::stable_sort(extra.begin(), extra.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
            std::vector<Ev> merged;  // equal times: notes first (std::merge keeps the first range first)
            std::merge(evs.begin(), evs.end(), extra.begin(), extra.end(), std::back_inserter(merged),
                       [](const Ev& a, const Ev& b) { return a.t < b.t; });
            evs.swap(merged);
        }
        std::vector<HostChange> host;
        for (const auto& item : hostArg.empty() ? std::vector<std::string>{} : split(hostArg, ',')) {
            const auto f = split(item, ':');
            if (f.size() < 3) throw std::runtime_error("bad --host item " + item);
            host.push_back({std::atof(f[0].c_str()), std::atof(f[1].c_str()), std::atoi(f[2].c_str()) != 0,
                            f.size() > 3, f.size() > 3 ? std::atof(f[3].c_str()) : 0.0});
        }
        const bool hostMode = !hostArg.empty();
        if (hostMode) player.resume();
        else player.syncTempo(120.0, 120.0 / 60.0 / sr, 4.0, 0.0, 0.0, 0);  // as part_oracle --tempo 120

        const int64_t total = int64_t((end + tail) * sr);
        std::vector<float> lr(size_t(total) * 2, 0.f);
        // host model (same arithmetic as part_oracle.py): ppq advances while playing
        double hBpm = 120.0, hPpq = 0.0;
        bool hPlay = false;
        int64_t hT = 0;
        size_t hi = 0;
        std::vector<hs2::MidiEvent> blockEvs;
        size_t ei = 0;
        for (int64_t t = 0; t < total;) {
            const int64_t n0 = std::min<int64_t>(block - t % block, total - t);
            hs2::HostTransport tr;
            if (hostMode) {
                while (hi < host.size() && int64_t(std::floor((lead + host[hi].t) * sr)) <= t) {
                    if (hPlay) hPpq = hPpq + double(t - hT) * (hBpm / 60.0 / sr);
                    hT = t;
                    hBpm = host[hi].bpm;
                    hPlay = host[hi].play;
                    if (host[hi].jump) hPpq = host[hi].ppq;
                    ++hi;
                }
                tr.bpm = hBpm;
                tr.playing = hPlay;
                tr.ppq = hPlay ? hPpq + double(t - hT) * (hBpm / 60.0 / sr) : hPpq;
                tr.barStart = std::floor(tr.ppq / 4.0) * 4.0;
            }
            blockEvs.clear();
            int64_t n2 = n0;  // a setParam event ends the block early (the part render is split there, as the oracle)
            for (size_t j = ei; j < evs.size() && evs[j].t < t + n0; ++j)
                if (evs[j].par >= 0) {
                    n2 = std::max<int64_t>(evs[j].t - t, 0);
                    break;
                }
            if (n2 == 0) {  // events up to and including the setParam at this frame, in order, without rendering
                const Ev& e = evs[ei++];
                if (e.par >= 0) player.setParam(e.par, e.idx, e.value);
                else player.midi(e.m[0], e.m[1], e.m[2], e.m[3]);
                continue;
            }
            const int64_t n = n2;
            while (ei < evs.size() && evs[ei].t < t + n) {
                hs2::MidiEvent e;
                e.offset = int(std::max<int64_t>(0, evs[ei].t - t));
                std::memcpy(e.data, evs[ei].m, 4);
                blockEvs.push_back(e);
                ++ei;
            }
            player.processBlock(&lr[size_t(t) * 2], int(n), blockEvs.data(), int(blockEvs.size()),
                                hostMode ? &tr : nullptr);
            t += n;
        }
        if (dry) undoEnhancer(lr, sr);
        writeWavFloat(out, lr, sr);
        std::printf("wrote %s\n", out.c_str());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
