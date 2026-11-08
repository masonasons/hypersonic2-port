// Arpeggiator / phrase sequencer: port of Hypersonic.dll 0x10045510..0x10048840 (see hs2/arp.h).
// Every method mirrors the disassembly (tools/emu/disasm.py), not the Ghidra decompile, which turns several double
// stores into float ones (FUN_10045540, FUN_10046980, FUN_100463a0 ...). The x87 runs at 53 bits: register values are
// double, rounded to float only where the original stores a float. Float->int conversions are fistp with the
// control word set to chop (truncation). Original quirks are kept and marked "quirk".
#include "hs2/arp.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

#include "hs2/bank.h"

namespace hs2 {

namespace {

// 0x103bfe20: per param {int steps, float default}; n = trunc((steps + 0.9f) * v)
constexpr int kParamSteps[17] = {1, 1, 15, 3, 16, 7, 20, 0, 0, 128, 128, 48, 128, 128, 128, 128, 12};
constexpr uint32_t kParamDefault[17] = {0, 0, 0, 0, 0, 0, 0x3f0ccccd, 0x3f000000, 0, 0x3f800000, 0x3f800000,
                                        0x3f000000, 0, 0x3f800000, 0, 0, 0};
// 0x103bfdc8: Rate -> step length in beats (index trunc(v*20.9) <= 20)
constexpr uint32_t kRates[21] = {0x41000000, 0x40e00000, 0x40c00000, 0x40a00000, 0x40800000, 0x40400000,
                                 0x402aaab9, 0x40000000, 0x3fc00000, 0x3faaaa8f, 0x3f800000, 0x3f400000,
                                 0x3f2aaae3, 0x3f000000, 0x3ec00000, 0x3eaaaa3b, 0x3e800000, 0x3e400000,
                                 0x3e2aaac1, 0x3e000000, 0x3d800000};
// 0x100a9dc8 (preceded by the 16 bytes at 0x100a9db8, which FUN_10045bd0 reads for a note count of 0): patterns of
// four chord indices, [count-1 % 10][kind]
constexpr uint8_t kPatterns[16 + 160] = {
    101, 114, 98, 0, 42, 46, 42, 0, 72, 121, 112, 101, 114, 32, 0, 0,  // 0x100a9db8 ("rb", "*.*", "Hyper ")
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 1, 2, 3, 3, 3, 3,
    0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 1, 2, 3, 4, 3, 4, 0, 1, 0, 1, 1, 1, 1, 1, 2, 3, 2, 3, 4, 5, 4, 5,
    0, 1, 0, 1, 1, 1, 1, 1, 2, 3, 4, 3, 5, 6, 5, 6, 0, 1, 0, 1, 1, 1, 1, 1, 2, 3, 4, 3, 5, 6, 7, 6,
    0, 1, 2, 1, 1, 2, 1, 2, 3, 4, 5, 4, 6, 7, 8, 7, 0, 1, 2, 1, 1, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 8};

constexpr float kThird = 0.33333334f;      // 0x100a9e90 (and -0.33333334f at 0x100a9ea0)
constexpr double kThirdD = 1.0 / 3.0;      // 0x100a9e98
constexpr float kInv127 = 0.007874016f;    // 0x100a7394
constexpr float kRandScale = 3.051851e-05f;  // 0x100a9e80 (1/32767)
constexpr size_t kPhrasePad = 0x18 * 64;   // zero padding after the phrase data (see hs2/arp.h)

inline float bitsToFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline int ring(int x) { return x % 32; }  // "and 0x8000001f" with the sign fix = C remainder
inline bool odd(int x) { return (x % 2) != 0; }
// The note folding the generator applies (cmp 2 / cmp 0x7f blocks; the divisions are unsigned)
inline int foldLow(int n) {
    if (n < 2) n = n + 12 + int((uint32_t(1 - n) / 12u) * 12u);
    return n;
}
inline int foldHigh(int n) {
    if (n > 0x7f) n = n - 12 - int((uint32_t(n - 0x80) / 12u) * 12u);
    return n;
}
inline int chop(double x) { return int(x); }  // fistp dword with RC = chop (values in range here)

}  // namespace

Arp::Arp(MsvcRand& rnd) : rnd_(&rnd) { init(); }

// ---- FUN_100467b0 -------------------------------------------------------------------------------------------
void Arp::init() {
    std::memset(&s_, 0, sizeof s_);
    s_.songPos = 0.0;
    s_.samplesPerBeat = 22050.0;
    s_.beatsPerSample = 4.5351473922902495e-05;  // 0x100a73c0
    s_.sampleRate = 44100.f;
    for (int i = 0; i < 17; ++i) s_.params[i] = bitsToFloat(kParamDefault[i]);
    for (int i = 0; i < 32; ++i) {
        s_.offNote[i] = 0;
        s_.offTime[i] = 0.0;
    }
    s_.offCount = s_.offHead = 0;
    s_.outCount = s_.outHead = 0;
    s_.nextIsNoteOff = 0;
    s_.phrase = 0;
    s_.pendingPhrase = 0;
    s_.sustain = 0;
    s_.playing = s_.phraseDrum = s_.phraseThru = 0;
    s_.stepTable = 1;  // original: the object's own address
    std::memset(s_.lastNote, 0xff, 16);
    s_.swungTime = -1.0;
    s_.phraseFlush = 0;
    s_.chordIndex = 0;
    s_.octaveShift = 0;
    s_.swingPending = -1.f;
    std::memset(s_.pattern, 0, 4);
    s_.patPos[2] = s_.patPos[1] = s_.patPos[0] = 0;
    s_.patKind = 0;
    s_.patActive = 0;
    s_.extraTranspose = 0;
    s_.bounceDown = 0;
    s_.phraseScale = 1.f;
    phraseBuf_.clear();
    pendingBuf_.clear();
}

// ---- FUN_10046340 -------------------------------------------------------------------------------------------
void Arp::setSampleRate(float sr) {
    s_.sampleRate = sr;
    for (int i = 0; i < 17; ++i) setParam(i, s_.params[i]);
    s_.w444 = -1;
    s_.running = 0;
    s_.noteCount = 0;
    s_.noteDirty = 0;
}

// ---- FUN_100460b0 (jump table 0x100462f8) ---------------------------------------------------------------------
void Arp::setParam(int i, float v) {
    if (i < 0 || i > 0x10) return;  // the part only passes 0..0x10 (the original would write past params[])
    s_.params[i] = v;
    const int n = chop((double(kParamSteps[i]) + double(0.9f)) * double(v));
    int e = n;
    if (i == 0xb)
        e = n - 0x18;
    else if (i == 3)
        e = n + 1;
    switch (i) {
    case 0:  // On
        if (e & int(s_.on == 0)) pushEvent(0xb0, 0x7b, 0, 0);  // quirk: "test edi, eax" is a bit test
        s_.on = e != 0;
        if (!s_.on) s_.stopRequest = 1;
        break;
    case 1:  // Latch
        if (e) {
            s_.latch = 1;
        } else {
            s_.releasePending = 1;
            s_.latch = 0;
        }
        break;
    case 2:  // Mode
        s_.mode = e;
        if (s_.running && e == 9 && (!s_.phrase || phraseCount() == 0)) s_.stopRequest = 1;
        break;
    case 3: s_.octaves = e; break;
    case 4: s_.numSteps = e; break;
    case 5: s_.syncMode = e; break;
    case 6: {  // Rate
        const float rate = bitsToFloat(kRates[e]);
        s_.phraseScale = float(1.0 / (double(0.5f) / double(rate)));
        if (s_.running && rate != s_.stepLen) retime(rate);
        s_.stepLen = rate;
        s_.gateLen = float(double(rate) * double(s_.params[7]) + double(0.001f));
        break;
    }
    case 7:  // Gate
        s_.gateLen = float(double(v) * double(s_.stepLen) + double(0.001f));
        break;
    case 8:  // Swing
        if (v != s_.swing) {
            if (s_.running)
                s_.swingPending = v;
            else
                s_.swing = v;
        }
        break;
    case 0xb: s_.transpose = e; break;
    case 0xc: s_.splitKey = e; break;
    case 0xd: s_.hiKey = e; break;
    case 0x10: s_.scaleKey = e == 0 ? 12 : e - 1; break;
    default: break;  // 9, 10 (phrase bank/prog: handled by the part), 14, 15: stored only
    }
}

// ---- FUN_100121a0 default case, 0xac..0xfb (jump table 0x100137d4) ------------------------------------------------
void Arp::setStep(int idx, float v) {
    if (idx < 0xac || idx > 0xfb) return;
    const int r = (idx - 0xac) / 5, f = (idx - 0xac) % 5;
    ArpState::Step& st = s_.steps[r];
    switch (f) {
    case 0: st.note = v; break;
    case 1: st.vel = float(double(v) + double(v)); break;
    case 2: st.gate = v; break;
    case 3: st.seq1 = chop(double(v) * double(126.9f)); break;
    case 4: st.seq2 = chop(double(v) * double(126.9f)); break;
    }
}

// ---- phrase loading: setter case 0x1b -> FUN_1001afb0(6, bank, prog) -> FUN_10016e20(0) -> FUN_10045510 ------------
std::vector<uint8_t> Arp::loadPhraseEntry(const std::filesystem::path& contentRoot, int bank, int prog) {
    std::error_code ec;
    std::filesystem::path file;
    for (const auto& de : std::filesystem::directory_iterator(contentRoot / "Phrases", ec)) {
        const std::string name = de.path().filename().string();
        if (name.size() >= 4 && std::isdigit((unsigned char)name[0]) && std::isdigit((unsigned char)name[1]) &&
            std::isdigit((unsigned char)name[2]) && name[3] == ' ' && std::stoi(name.substr(0, 3)) == bank &&
            de.is_regular_file(ec)) {
            file = de.path();
            break;
        }
    }
    if (file.empty()) return {};
    std::unique_ptr<MilanBank> b;
    try {
        b = MilanBank::open(file);
    } catch (const std::exception&) {
        return {};
    }
    if (prog < 0 || prog >= int(b->entries.size())) return {};
    auto data = b->read(b->entries[size_t(prog)].slot1);
    // as the original's reader (FUN_10083a50) returns it: the payload after any 'SyncrosoftContentHeader' blob
    while (data.size() >= 31 && std::memcmp(data.data(), "SyncrosoftContentHeader", 23) == 0) {
        const size_t skip = 8 + size_t(rd32(data.data() + 27));
        if (skip > data.size()) return {};
        data = data.subspan(skip);
    }
    std::vector<uint8_t> obj(data.begin(), data.end());
    // setter @100134d0: bank 10 (Expression) marks its first event "thru" (byte 3 = 1) when it has events
    if (bank == 10 && obj.size() >= 0x20c && rd32(obj.data() + 0x1f4) != 0) obj[0x20b] = 1;
    return obj;
}

bool Arp::setPhrase(const std::filesystem::path& contentRoot, int bank, int prog) {
    if (bank + prog == 0) return false;
    const auto obj = loadPhraseEntry(contentRoot, bank, prog);
    if (obj.empty()) return false;
    setPhraseObject(obj);
    return true;
}

void Arp::setPhraseObject(std::span<const uint8_t> obj) {
    std::vector<uint8_t> buf(obj.size() > 0x1f0 ? obj.size() - 0x1f0 + kPhrasePad : kPhrasePad, 0);
    if (obj.size() > 0x1f0) std::memcpy(buf.data(), obj.data() + 0x1f0, obj.size() - 0x1f0);
    pendingBuf_ = std::move(buf);
    s_.pendingPhrase = 1;
}

Arp::PatchTrailer Arp::patchTrailer(std::span<const uint8_t> blob) {
    PatchTrailer t;
    const uint8_t* d = blob.data();
    size_t n = blob.size();
    if (n >= 0x40 && std::memcmp(d, "StSS", 4) != 0 && std::memcmp(d + 0x3c, "StSS", 4) == 0) {
        d += 0x3c;  // inside a VST fxp/fxb chunk
        n -= 0x3c;
    }
    if (n < 0x620) return t;
    int nfx = 0;
    for (int i = 0; i < 4; ++i) nfx += d[0x18 + i] != 0;
    size_t o = 0x620 + (size_t(rd32(d + 0x14)) + size_t(nfx)) * 0x420;
    if (o + 4 > n) return t;
    t.kind = rd32(d + o);
    if (t.kind != 2) return t;
    o += 4 + 0x100;
    if (o + 4 > n) return t;
    t.lastSlot = int32_t(rd32(d + o));
    o += 4;
    for (int i = 0; i <= t.lastSlot && o + 4 <= n; ++i) {
        const uint32_t size = rd32(d + o);
        if (size == 0) {
            t.slots.emplace_back();
            o += 4;
            continue;
        }
        if (o + size > n) break;
        t.slots.emplace_back(d + o, size);
        o += size;
    }
    return t;
}

float Arp::phraseLen() const {
    float f;
    std::memcpy(&f, phraseBuf_.data(), 4);
    return f;
}
int32_t Arp::phraseCount() const {
    int32_t n;
    std::memcpy(&n, phraseBuf_.data() + 4, 4);
    return n;
}
void Arp::setPhraseCount(int32_t n) { std::memcpy(phraseBuf_.data() + 4, &n, 4); }
double Arp::evTime(int i) const {
    const size_t o = 8 + size_t(i) * 0x18;
    if (i < 0 || o + 8 > phraseBuf_.size()) return 0.0;
    double d;
    std::memcpy(&d, phraseBuf_.data() + o, 8);
    return d;
}
double Arp::evLength(int i) const {
    const size_t o = 0x10 + size_t(i) * 0x18;
    if (i < 0 || o + 8 > phraseBuf_.size()) return 0.0;
    double d;
    std::memcpy(&d, phraseBuf_.data() + o, 8);
    return d;
}
uint8_t Arp::evByte(int i, int b) const {
    const size_t o = 0x18 + size_t(i) * 0x18 + size_t(b);
    if (i < 0 || o >= phraseBuf_.size()) return 0;
    return phraseBuf_[o];
}

// ---- FUN_10045540(stepTab, phrase): take the pending phrase (called by next() with stepTab 0) --------------------
void Arp::applyPhrase(int stepTab, bool hasData) {
    if (stepTab != 0) s_.stepTable = 1;
    if (!hasData) return;
    std::swap(phraseBuf_, pendingBuf_);  // the original just takes the pointer; the old object stays alive
    if (phraseLen() < 0.f || phraseCount() < 0 || phraseCount() > 1000) setPhraseCount(0);
    s_.phrase = 1;
    if (s_.running && s_.mode > 4 && s_.stepTable == 0) {  // dead in practice (+0x3f0 is never 0)
        s_.stopRequest = 1;
        return;
    }
    s_.phraseDrum = phraseCount() != 0 && evByte(0, 0) == 0x99;
    s_.phraseThru = phraseCount() != 0 && evByte(0, 3) == 1;
    if (!s_.running || s_.mode <= 8) return;
    // running phrase replaced: continue from the current position in the new phrase
    s_.phraseFlush = 1;
    double d = s_.songPos - s_.phraseStart;
    if (d > double(phraseLen()) * double(s_.phraseScale)) {
        do {
            d -= 4.0;  // quirk: steps back 4 beats, not the phrase length
        } while (d > double(phraseLen()) * double(s_.phraseScale));
    }
    const double sc = double(s_.phraseScale);
    const double lim = d - double(0.01f);
    for (int i = 0; i < phraseCount(); ++i) {
        s_.stepIndex = -1;
        if (sc * evTime(i) > lim) {
            const double t = sc * evTime(i) + s_.phraseStart;
            s_.nextTime = t;
            if (t < s_.songPos) s_.nextTime = s_.songPos;
            s_.stepIndex = i;
            break;
        }
    }
    if (s_.stepIndex < 0) {
        s_.stepIndex = 0;
        const double ps = double(phraseLen()) * double(s_.phraseScale) + s_.phraseStart;
        s_.phraseStart = ps;
        s_.nextTime = double(s_.phraseScale) * evTime(0) + ps;
    }
}

// ---- FUN_10045710 --------------------------------------------------------------------------------------------
void Arp::sync(int flags, double bpm, double ppq) {
    s_.playing = uint8_t(flags & 1);
    const double spb = double(s_.sampleRate) / bpm * 60.0;
    s_.samplesPerBeat = spb;
    s_.beatsPerSample = 1.0 / spb;
    if (flags & 2) {
        s_.stopRequest = 1;
        return;
    }
    if (flags & 0xc) {
        const double d = ppq - s_.songPos;
        s_.nextTime = d + s_.nextTime;
        s_.stepTime = d + s_.stepTime;
        s_.phraseStart = d + s_.phraseStart;
        s_.songPos = ppq;
        for (int i = 0; i < s_.offCount; ++i) {
            double& t = s_.offTime[ring(s_.offHead + i)];
            t = d + t;
        }
        return;
    }
    s_.songPos = ppq;
}

// ---- small helpers ---------------------------------------------------------------------------------------------
void Arp::pushEvent(int b0, int b1, int b2, int b3) {  // FUN_100457e0
    uint8_t* e = s_.out[ring(s_.outCount + s_.outHead)];
    e[0] = uint8_t(b0);
    e[1] = uint8_t(b1);
    e[2] = uint8_t(b2);
    e[3] = uint8_t(b3);
    s_.outCount += 1;
}

void Arp::addNoteOff(int note, double t) {  // FUN_10045880
    const int k = ring(s_.offCount + s_.offHead);
    s_.offTime[k] = t;
    s_.offNote[k] = uint8_t(note);
    s_.offCount += 1;
}

bool Arp::isNewNote(int note, int last) const {  // FUN_100458e0: false = the note is still sounding
    if (uint8_t(note) != uint8_t(last)) return true;
    const int n = int(int8_t(note));
    for (int i = 0, h = s_.offHead; i < s_.offCount; ++i, ++h)
        if (int(s_.offNote[ring(h)]) == n) return false;
    return true;
}

void Arp::extendNoteOff(int note, float add) {  // FUN_10045950
    const int n = int(int8_t(note));
    for (int i = 0, h = s_.offHead; i < s_.offCount; ++i, ++h) {
        if (int(s_.offNote[ring(h)]) == n) {
            double& t = s_.offTime[ring(s_.offHead + i)];
            t = double(add) + t;
            return;
        }
    }
}

int Arp::blackKey(int note) const {  // FUN_100459d0
    if (s_.scaleKey == 12) return 0;
    const int r = (note - s_.scaleKey + 12) % 12;
    const uint32_t d = uint32_t(r - 1);
    if (d > 9) return 0;
    static constexpr uint8_t kBlack[10] = {1, 0, 1, 0, 0, 1, 0, 1, 0, 1};
    return kBlack[d];
}

int Arp::phraseVel(int evVel, int noteVel, bool raw) {  // FUN_10045a30 (callers use the low byte)
    if (raw) return noteVel & 0xff;
    const int v = chop(double(int(int8_t(evVel)) * int(int8_t(noteVel))) * double(0.015625f));
    return v > 0x7f ? 0x7f : v;
}

int Arp::stepVel(float scale, int vel) {  // FUN_10045a90
    const int v = chop(double(int(int8_t(vel))) * double(scale));
    if (v > 0x7f) return 0x7f;
    if (v < 1) return scale == 0.f ? 0 : 1;
    return v;
}

double Arp::stepGate(float g2, float gate) const {  // FUN_10045b00
    double x = double(g2) * double(gate);
    if (x > 1.0) x = 1.0;
    return x * double(s_.stepLen) + double(0.001f);
}

bool Arp::chance(bool on, int noteVel, int evVel) {  // FUN_10045b30
    if (!on) return true;
    const int r = rnd_->next();
    const double rc = double(r) * double(kRandScale);
    const double p = 1.0 - double(int(int8_t(noteVel))) * double(kInv127);
    float a = float(1.0 - p * p);
    double b = double(int(int8_t(evVel))) * double(kInv127);
    if (int8_t(evVel) == 0x7f || int8_t(noteVel) == 0x7f) {
        a = 1.f;
        b = 1.0;
    }
    return rc < double(a) * b;
}

void Arp::loadPattern(int kind, int count) {  // FUN_10045bd0
    const int idx = kind + ((count - 1) % 10) * 4;
    const int o = 16 + idx * 4;  // kPatterns starts 16 bytes before the original table
    for (int k = 0; k < 4; ++k) {
        const int p = o + k;
        s_.pattern[k] = int8_t(p >= 0 && p < int(sizeof kPatterns) ? kPatterns[p] : 0);
    }
}

// FUN_10045c30(newLen): a Rate change while running re-times the next step (called before stepLen is updated)
void Arp::retime(float newLen) {
    if (s_.mode < 9) {
        const double d = s_.songPos - s_.phraseStart;
        const double len2 = double(newLen) + double(newLen);
        double t = d - std::fmod(d, len2);
        if (odd(s_.stepIndex)) t = t + (double(s_.swing) * double(kThird) + double(1.f)) * double(newLen);
        const double thr = double(s_.stepLen) * double(0.5f) + d;
        while (t < thr) t = len2 + t;
        t = t + s_.phraseStart;
        if (s_.nextIsNoteOff == 0) {
            s_.nextTime = t;
            s_.stepTime = t;
            return;
        }
        s_.swungTime = t;
        if (t < s_.nextTime) s_.nextTime = t - double(0.05f);
    } else {
        const double q = double(newLen) / double(s_.stepLen);
        s_.phraseStart = s_.nextTime - (s_.nextTime - s_.phraseStart) * q;
    }
}

void Arp::sortNoteOffs() {  // FUN_10045d50
    if (s_.offCount < 2) return;
    for (int i = 0; i < s_.offCount; ++i) {
        for (int j = i + 1; j < s_.offCount; ++j) {
            const int a = ring(s_.offHead + i), b = ring(s_.offHead + j);
            if (s_.offTime[a] > s_.offTime[b]) {
                const double t = s_.offTime[b];
                const uint8_t n = s_.offNote[b];
                s_.offTime[b] = s_.offTime[a];
                s_.offNote[b] = s_.offNote[a];
                s_.offTime[a] = t;
                s_.offNote[a] = n;
            }
        }
    }
}

double Arp::phraseSwing(double t) const {  // FUN_10045ea0
    double r = std::fmod(t, 0.5);
    if (!(r <= 0.25)) r = 0.5 - r;
    return r * kThirdD * double(s_.swing);
}

// ---- note list (object at +0x1b8) ---------------------------------------------------------------------------------
void Arp::noteListRemove(uint8_t note, bool keep) {  // FUN_10045ee0
    uint8_t* L = raw() + 0x1b8;
    int32_t& count = s_.noteCount;
    int i = 0;
    if (count > 0) {
        for (; i < count; ++i)
            if (L[5 + 3 * i] == note) break;
        if (i < count) {
            if (keep) {
                L[7 + 3 * i] = 0;
                L[4] = 1;
            } else {
                for (int k = i; k < count - 1; ++k) std::memcpy(L + 5 + 3 * k, L + 8 + 3 * k, 3);
            }
        }
    }
    if (i == count) return;
    int j = 0;
    for (; j < count; ++j)
        if (L[0x35 + 3 * j] == note) break;
    if (j < count) {
        if (keep) {
            L[0x37 + 3 * j] = 0;
            L[4] = 1;
        } else {
            for (int k = j; k < count - 1; ++k) std::memcpy(L + 0x35 + 3 * k, L + 0x38 + 3 * k, 3);
        }
    }
    if (!keep) count -= 1;
}

void Arp::noteListCompact() {  // FUN_10045fd0
    uint8_t* L = raw() + 0x1b8;
    int n = s_.noteCount;  // the played list uses a local count; the sorted list then decrements the real one
    for (int i = 0; i < n; ++i) {
        if (L[7 + 3 * i] == 0) {
            n -= 1;
            if (i < n)
                for (int b = 0; b < (n - i) * 3; ++b) L[5 + 3 * i + b] = L[8 + 3 * i + b];  // rep movsb
            i -= 1;
        }
    }
    for (int j = 0; j < s_.noteCount; ++j) {
        if (L[0x37 + 3 * j] == 0) {
            s_.noteCount -= 1;
            for (int k = j; k < s_.noteCount; ++k) std::memcpy(L + 0x35 + 3 * k, L + 0x38 + 3 * k, 3);
            j -= 1;
        }
    }
    L[4] = 0;
}

void Arp::noteListAdd(uint8_t note, uint8_t vel) {  // FUN_10046700
    uint8_t* L = raw() + 0x1b8;
    if (s_.noteCount == 16) return;
    if (L[4]) noteListCompact();
    const int count = s_.noteCount;
    int i = 0;
    for (; i < s_.noteCount; ++i) {
        if (L[0x35 + 3 * i] >= note) {
            if (L[0x35 + 3 * i] == note) return;
            for (int k = count; k > i; --k) std::memcpy(L + 0x35 + 3 * k, L + 0x32 + 3 * k, 3);
            break;
        }
    }
    L[0x35 + 3 * i] = note;
    L[0x37 + 3 * i] = 1;
    L[0x36 + 3 * i] = vel;
    const int c = s_.noteCount;
    L[5 + 3 * c] = note;
    L[6 + 3 * c] = vel;
    L[7 + 3 * c] = 1;
    s_.noteCount += 1;
}

// ---- FUN_100463a0(prev): time of the next action (pending note-off or step) --------------------------------------
double Arp::nextActionTime(double prev) {
    double t;
    if (s_.mode < 9) {
        const bool o = odd(s_.stepIndex);
        t = double(s_.stepLen) + s_.stepTime;
        if (o && s_.swingPending > -1.f) {
            s_.swing = s_.swingPending;
            s_.swingPending = -1.f;
        }
        if (s_.swungTime > -1.0) {
            t = s_.swungTime;
        } else {
            t = t + double(s_.swing) * double(s_.stepLen) * (o ? double(kThird) : double(-kThird));
        }
        if (s_.offCount != 0 && t >= s_.offTime[s_.offHead]) {
            s_.nextIsNoteOff = 1;
            return s_.offTime[s_.offHead];
        }
        s_.stepTime = t;
        s_.nextIsNoteOff = 0;
        s_.swungTime = -1.0;
        return t;
    }
    if (!s_.phrase) return prev + 1.0;
    if (s_.stepIndex >= phraseCount()) s_.stepIndex = 0;
    if (phraseCount() == 0) return prev + 1.0;
    if (s_.swingPending > -1.f) {
        s_.swing = s_.swingPending;
        s_.swingPending = -1.f;
    }
    const double et = evTime(s_.stepIndex);
    t = (phraseSwing(et) + et) * double(s_.phraseScale) + s_.phraseStart;
    if (s_.offCount != 0 && t >= s_.offTime[s_.offHead]) {
        s_.nextIsNoteOff = 1;
        return s_.offTime[s_.offHead];
    }
    s_.stepTime = t;
    s_.nextIsNoteOff = 0;
    return t;
}

// ---- FUN_10046550(ev, note, scale, vel): one phrase note (on now, off at the event's end) ---------------------------
void Arp::phraseNote(int ev, int note, bool scale, int vel) {
    uint8_t n = uint8_t(note);
    if (int8_t(n) < 0) {
        const uint8_t a = uint8_t(0xff - n);
        const uint8_t m = uint8_t((a / 12 + 1) * 12);
        n = uint8_t(m + n);
    }
    if (scale) n = uint8_t(n + blackKey(int(int8_t(n))));
    pushEvent(0x90, n, vel, 0);
    auto offAt = [&]() {
        const double end = evLength(ev) + evTime(ev);
        double r = std::fmod(end, 0.5);
        if (!(r <= 0.25)) r = 0.5 - r;
        const double r3 = r * kThirdD;
        return (double(s_.swing) * r3 + end) * double(s_.phraseScale) + s_.phraseStart;
    };
    const int k = ring(s_.offHead + s_.offCount);
    s_.offTime[k] = offAt();
    s_.offNote[ring(s_.offHead + s_.offCount)] = n;
    s_.offCount += 1;
    sortNoteOffs();
    if (s_.extraTranspose != 0) {
        const uint8_t n2 = uint8_t(n + uint8_t(s_.extraTranspose));
        if (int8_t(n2) >= 0) {
            pushEvent(0x90, n2, vel, 0);
            addNoteOff(int(int8_t(n2)), offAt());
            sortNoteOffs();
        }
    }
}

// ---- FUN_10046980(msg) ---------------------------------------------------------------------------------------------
void Arp::patternUpdate(int count) {  // the mode-9 block shared by note-on and note-off
    if (count - 1 < s_.chordIndex) s_.chordIndex = count - 1;
    if (s_.patActive) {
        loadPattern(s_.patKind, count);
        int p;
        if (s_.patKind == 2)
            p = s_.patPos[1];
        else if (s_.patKind == 3)
            p = s_.patPos[2];
        else
            p = s_.patPos[0];
        s_.chordIndex = int(int8_t(byteAt(0x450 + p)));
    }
}

bool Arp::midiIn(const uint8_t msg[4]) {
    const uint8_t st = msg[0];
    bool noteOff = false;
    if (st == 0x80) {
        noteOff = true;
    } else if (st == 0xb0) {
        const uint8_t cc = msg[1];
        if (cc == 0x40) {
            if (!s_.on || s_.mode == 7) return false;
            if (msg[2] > 0x3f) {
                s_.sustain = 1;
                return true;
            }
            s_.sustain = 0;
            if (s_.running) s_.releasePending = 1;
            return false;
        }
        if (cc == 0x78 || cc == 0x7b) {
            s_.stopRequest = 1;
            s_.noteCount = 0;
            s_.noteDirty = 0;
        }
        return false;
    } else if (st == 0x90) {
        if (!s_.on) return false;
        if (msg[2] == 0) noteOff = true;
    } else {
        return false;
    }

    if (noteOff) {  // @10046c98
        const bool keep = (s_.latch | s_.sustain) != 0;
        noteListRemove(msg[1], keep);
        const int count = s_.noteCount;
        if (s_.mode == 9 && s_.latch == 0 && count > 0) patternUpdate(count);
        if (s_.noteCount == 0) s_.stopRequest = 1;
        if (s_.mode == 8 && int(msg[1]) < s_.splitKey) return false;
        if (s_.phrase && s_.mode == 9 && s_.phraseThru) return false;
        if (!s_.on) return false;
        return s_.mode != 7;
    }

    // note on
    if (s_.mode == 8 && int(msg[1]) < s_.splitKey) return false;
    noteListAdd(msg[1], msg[2]);
    if (s_.mode == 9) patternUpdate(s_.noteCount);
    if (!s_.running || s_.syncMode == 1) {
        const int m = s_.mode;
        if (m > 8 && !s_.phrase) return true;
        if (m > 4 && !s_.stepTable) return true;
        const int sync = s_.syncMode;
        s_.nextTime = s_.songPos;
        s_.running = 1;
        s_.phraseStart = s_.songPos;
        s_.stopRequest = 0;
        s_.releasePending = 0;
        s_.stepIndex = 0;
        if (sync >= 0) {
            if (sync <= 1) {
                if (m > 8) s_.nextTime = double(s_.phraseScale) * evTime(0) + s_.nextTime;
            } else if (sync == 2) {
                if (s_.playing) {
                    if (m < 9) {
                        const double r = std::fmod(s_.songPos, double(s_.stepLen));
                        s_.nextTime = r + s_.nextTime;
                        s_.phraseStart = r + s_.phraseStart;
                    } else {
                        const double r = std::fmod(s_.songPos, 0.25);
                        s_.nextTime = double(s_.phraseScale) * evTime(0) + s_.nextTime + r;
                        s_.phraseStart = r + s_.phraseStart;
                    }
                } else if (m > 9) {
                    s_.nextTime = double(s_.phraseScale) * evTime(0) + s_.nextTime;
                }
            }
        }
        s_.nextIsNoteOff = 0;
        s_.stepTime = s_.nextTime;
        std::memset(s_.lastNote, 0xff, 16);
        s_.swungTime = -1.0;
        std::memset(s_.pattern, 0, 4);
        s_.patPos[2] = s_.patPos[1] = s_.patPos[0] = 0;
        s_.patActive = 0;
        s_.patKind = 0;
        s_.chordIndex = 0;
        s_.bounceDown = 0;
    }
    if (s_.mode == 7) return false;
    if (s_.phrase && s_.mode == 9 && s_.phraseThru) return false;
    return true;
}

// ---- FUN_10046da0: one step ---------------------------------------------------------------------------------------
// Modes 0..4 after the note is chosen (@10047001 / @10046f0f): fold, scale, step CCs, swing, note on / extend.
void Arp::arpTail(int note, int vel) {
    int n = foldHigh(note);
    n += blackKey(n);
    if (s_.numSteps != 0) {
        const int r = s_.stepIndex % s_.numSteps;
        pushEvent(0xb0, 0x80, uint8_t(s_.steps[r].seq1), 0);
        pushEvent(0xb0, 0x81, uint8_t(s_.steps[r].seq2), 0);
    }
    const double sw = double(s_.swing) * double(s_.gateLen) * (odd(s_.stepIndex) ? double(-kThird) : double(kThird));
    if (!isNewNote(n, s_.lastNote[0])) {
        extendNoteOff(n, float(sw + double(s_.gateLen)));
    } else {
        pushEvent(0x90, n, vel, 0);
        addNoteOff(n, (sw + double(s_.gateLen)) + s_.nextTime);
    }
    s_.lastNote[0] = uint8_t(n);
}

// Modes 5 (Step Seq) and 6 (Rand Seq), record `rec`
void Arp::stepSeq(int rec, bool randomBug, int ri) {
    const ArpState::Step& st = s_.steps[rec];
    const float g2 = float(double(st.gate) + double(st.gate));
    const float gl = float(stepGate(g2, s_.params[7]));
    pushEvent(0xb0, 0x80, uint8_t(st.seq1), 0);
    pushEvent(0xb0, 0x81, uint8_t(st.seq2), 0);
    for (int k = 0; k < s_.noteCount; ++k) {
        const int vel = int(int8_t(stepVel(st.vel, s_.played[k][1])));
        int n = chop(double(st.note) * double(48.f) + double(0.5f)) + int(s_.played[k][0]) + s_.transpose - 0x18;
        n = foldHigh(foldLow(n));
        n += blackKey(n);
        const double sw = double(gl) * double(s_.swing) * (odd(s_.stepIndex) ? double(-kThird) : double(kThird));
        if (isNewNote(n, s_.lastNote[k])) {
            pushEvent(0x90, n, vel, 0);
            // quirk (Rand Seq @10047465): the note-off is queued for the random step index, not the note
            addNoteOff(randomBug ? ri : n, (double(gl) + sw) + s_.nextTime);
        } else {
            extendNoteOff(n, float(double(gl) + sw));
        }
        s_.lastNote[k] = uint8_t(n);
    }
}

void Arp::step() {
    const int count = s_.noteCount;
    const int mode = s_.mode;
    switch (mode) {
    case 0:    // Up (sorted list)
    case 1:    // Down
    case 3: {  // As Played
        if (count == 0) return;  // the original divides by zero (cannot happen: see hs2/arp.h)
        const int si = s_.stepIndex;
        int idx, oct;
        if (mode == 1) {
            idx = count - si % count - 1;
            oct = s_.octaves - (si / count) % s_.octaves - 1;
        } else {
            idx = si % count;
            oct = (si / count) % s_.octaves;
        }
        // computed offsets (as the original indexes the object)
        const int base = mode == 3 ? 0x1bd : 0x1ed;
        const int vel = byteAt(base + 3 * idx + 1);
        const int n = foldLow(int(byteAt(base + 3 * idx)) + oct * 12);
        arpTail(n, vel);
        break;
    }
    case 2:    // Up+Down
    case 4: {  // Random
        if (count == 0) return;
        int idx, oct;
        if (mode == 2) {
            const int oc = s_.octaves;
            const int span = oc * count;
            int len = span * 2 - 2;
            if (len < 1) len = 1;
            int p = s_.stepIndex % len;
            if (p >= span) p = len - p;
            idx = p % count;
            oct = (p / count) % oc;
        } else {
            oct = rnd_->next() % s_.octaves;
            idx = rnd_->next() % count;
        }
        const int vel = byteAt(0x1ed + 3 * idx + 1);
        const int n = foldLow(int(byteAt(0x1ed + 3 * idx)) + oct * 12);
        arpTail(n, vel);
        break;
    }
    case 5:  // Step Seq
        stepSeq(s_.stepIndex & 0xf, false, 0);
        break;
    case 6: {  // Rand Seq
        const int ns = s_.numSteps != 0 ? s_.numSteps : 16;
        const int ri = rnd_->next() % ns;
        stepSeq(ri, true, ri);
        break;
    }
    case 7: {  // Ctrl Seq: the step CCs only
        const ArpState::Step& st = s_.steps[s_.stepIndex & 0xf];
        pushEvent(0xb0, 0x80, uint8_t(st.seq1), 0);
        pushEvent(0xb0, 0x81, uint8_t(st.seq2), 0);
        break;
    }
    case 8: {  // Elem Seq: the step note picks the element (note 35 + 16 note), byte 3 carries the played pitch
        const ArpState::Step& st = s_.steps[s_.stepIndex & 0xf];
        pushEvent(0xb0, 0x80, uint8_t(st.seq1), 0);
        pushEvent(0xb0, 0x81, uint8_t(st.seq2), 0);
        int n = chop(double(st.note) * double(16.f) + double(0.5f)) + 0x23;
        n = foldHigh(foldLow(n));
        // quirk: odd steps are shortened by Gate/3 whatever the swing (even steps lengthen by swing*Gate/3)
        const float sw = odd(s_.stepIndex) ? float(double(s_.gateLen) * double(-kThird))
                                           : float(double(s_.swing) * double(s_.gateLen) * double(kThird));
        for (int k = 0; k < count; ++k) {
            // quirk: compares the previous note of this voice with itself, so only the ring search counts, and
            // lastNote is never updated in this mode (stays 0xff: every step is a new note)
            if (isNewNote(s_.lastNote[k], s_.lastNote[k])) {
                const uint8_t d3 = uint8_t(s_.played[k][0] + uint8_t(s_.transpose) - 0x3c);
                const int vel = stepVel(st.vel, s_.played[k][1]);
                pushEvent(0x90, n, vel, d3);
                addNoteOff(n, (double(sw) + double(s_.gateLen)) + s_.nextTime);
            } else {
                extendNoteOff(n, float(double(sw) + double(s_.gateLen)));
            }
        }
        break;
    }
    case 9:
        if (!s_.phrase) return;  // jumps past the step increment
        phraseStep(count);
        break;
    default:
        if (!s_.phrase) return;  // the original dereferences the null phrase below
        break;
    }
    // @10048452
    s_.stepIndex += 1;
    if (s_.mode < 9) {
        if (s_.numSteps != 0 && s_.stepIndex >= s_.numSteps) s_.stepIndex = 0;
    } else if (s_.stepIndex > phraseCount() - 1) {
        s_.stepIndex = 0;
        s_.phraseStart = double(s_.phraseScale) * double(phraseLen()) + s_.phraseStart;
    }
}

// Mode 9 (@1004769f): the phrase event at stepIndex
void Arp::phraseStep(int count) {
    const int ei = s_.stepIndex;
    const uint8_t status = evByte(ei, 0);
    const int ch = status & 0xf;
    if ((status & 0xf0) != 0x90) {
        pushEvent(status, evByte(ei, 1), evByte(ei, 2), 0);
        return;
    }
    if (s_.phraseFlush) {
        s_.phraseFlush = 0;
        while (s_.offCount != 0) {
            pushEvent(0x80, s_.offNote[s_.offHead], 0, 0);
            s_.offCount += -1;
            s_.offHead = ring(s_.offHead + 1);
        }
    }
    const int saved = s_.extraTranspose;
    s_.extraTranspose = 0;
    const uint8_t d1 = evByte(ei, 1), d2 = evByte(ei, 2);
    switch (ch) {
    case 9:  // the event's note as is, velocity scaled by the lowest held note's
        phraseNote(ei, d1, false, phraseVel(d2, byteAt(0x1ee), false));
        return;
    case 10:  // relative to the lowest held note
        phraseNote(ei, d1 + byteAt(0x1ed) - 0x3c, true, phraseVel(d2, byteAt(0x1ee), false));
        return;
    case 11:  // relative to the highest
        phraseNote(ei, byteAt(0x1ea + 3 * count) + d1 - 0x3c, true, phraseVel(d2, byteAt(0x1eb + 3 * count), false));
        return;
    case 12:  // relative to the first played
        phraseNote(ei, d1 + byteAt(0x1bd) - 0x3c, true, phraseVel(d2, byteAt(0x1be), false));
        return;
    case 13:  // relative to the last played
        phraseNote(ei, byteAt(0x1ba + 3 * count) + d1 - 0x3c, true, phraseVel(d2, byteAt(0x1bb + 3 * count), false));
        return;
    case 14:
    case 15:
        break;
    default:  // 0..8: relative to every held note, in played order
        for (int k = 0; k < count; ++k)
            phraseNote(ei, evByte(ei, 1) - 0x3c + s_.played[k][0], true, phraseVel(evByte(ei, 2), s_.played[k][1], false));
        return;
    }

    // channels 14/15: chord commands (data1 0x24..0x5f; jump table 0x10048504 via 0x10048590)
    s_.extraTranspose = saved;
    const bool raw = ch == 15;  // channel 15: the held note's velocity as is (and chance() is on)
    const int cmd = d1;
    auto& ci = s_.chordIndex;
    auto noteVel = [&](int i) { return int(byteAt(0x1ee + 3 * i)); };
    auto noteKey = [&](int i) { return int(byteAt(0x1ed + 3 * i)); };
    auto emit = [&](int i) {  // @1004838e
        const int vel = phraseVel(evByte(s_.stepIndex, 2), noteVel(i), raw);
        phraseNote(s_.stepIndex, noteKey(i) + s_.octaveShift * 12, false, vel);
    };
    auto check = [&]() { return chance(raw, noteVel(ci), d2); };
    switch (cmd) {
    case 0x24:
        ci = 0;
        s_.patActive = 0;
        break;
    case 0x25:
        ci = 1;
        if (count - 1 < 1) ci = count - 1;
        s_.patActive = 0;
        break;
    case 0x26:
    case 0x27:
        ci = count - (cmd == 0x26 ? 1 : 2);
        if (ci < 0) ci = 0;
        s_.patActive = 0;
        break;
    case 0x28:
    case 0x29:
        ci += cmd == 0x28 ? 1 : 2;
        if (ci > count - 1) ci = ci - count;
        s_.patActive = 0;
        break;
    case 0x2a:
    case 0x2b:
        ci += cmd == 0x2a ? -1 : -2;
        if (ci < 0) ci = ci + count;
        s_.patActive = 0;
        break;
    case 0x2c:
    case 0x2d:
        ci += cmd == 0x2c ? 1 : 2;
        if (ci > count - 1) ci = ci - count;
        if (ci == 0) ci = 1;
        if (ci > count - 1) ci = count - 1;
        s_.patActive = 0;
        break;
    case 0x2e:
    case 0x2f:
        ci += cmd == 0x2e ? -1 : -2;
        if (ci < 0) ci = ci + count;
        if (ci == 0) ci = count - 1;
        s_.patActive = 0;
        break;
    case 0x30:
    case 0x31: {
        loadPattern(cmd - 0x30, count);
        const int p = s_.patPos[0];
        ci = int(int8_t(byteAt(0x450 + p)));
        s_.patPos[0] = int8_t((p + 1) % 4);
        if (cmd == 0x31 && count > 1 && ci == 0) ci = 1;
        s_.patActive = 1;
        s_.patKind = cmd - 0x30;
        break;
    }
    case 0x32:
    case 0x33: {
        loadPattern(cmd - 0x30, count);
        int8_t& pp = cmd == 0x32 ? s_.patPos[1] : s_.patPos[2];
        const int p = pp;
        ci = int(int8_t(byteAt(0x450 + p)));
        pp = int8_t((p + 1) % 4);
        s_.patActive = 1;
        s_.patKind = cmd - 0x30;
        break;
    }
    case 0x34: {
        const int r = rnd_->next();
        s_.patActive = 0;
        ci = chop(double(r) * double(kRandScale) * (double(count) - double(0.1f)));
        break;
    }
    case 0x35: {
        const int r = rnd_->next();
        ci = chop(double(r) * double(kRandScale) * (double(count) - double(1.1f))) + 1;
        if (count == 1) ci = 0;
        s_.patActive = 0;
        break;
    }
    case 0x36:
        s_.octaveShift += 1;
        if (s_.octaveShift > 1) s_.octaveShift = 1;
        break;
    case 0x37:
        s_.octaveShift += -1;
        if (s_.octaveShift < -1) s_.octaveShift = -1;
        break;
    case 0x38:
        s_.extraTranspose = int(d2) - 0x40;
        if (s_.extraTranspose > 0x18) s_.extraTranspose = 0x18;
        if (s_.extraTranspose < -0x18) s_.extraTranspose = -0x18;
        break;
    case 0x39:
        s_.extraTranspose = 0;
        break;
    case 0x3a:  // bounce
        s_.patActive = 0;
        if (s_.bounceDown == 0) {
            ci += 1;
            if (ci > count - 1) {
                ci = count - 2;
                s_.bounceDown = 1;
                if (count - 2 < 0) ci = 0;
            }
        } else {
            ci += -1;
            if (ci < 0) {
                ci = 1;
                if (count - 1 < 1) ci = 0;
                s_.bounceDown = 0;
            }
        }
        break;
    case 0x3c:
    case 0x3d:  // the current chord note
        if (check()) emit(ci);
        break;
    case 0x3e:
    case 0x3f:
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45: {  // the current chord note plus the next / next-but-one (wrapping, the 0x42.. forms skip index 0)
        if (!check()) break;
        emit(ci);
        if (count < 2) break;
        int j;
        if (cmd <= 0x3f) {
            j = ci + 1;
            if (j > count - 1) j = 0;
        } else if (cmd <= 0x41) {
            j = ci + 2;
            if (j > count - 1) j = j - count;
        } else if (cmd <= 0x43) {
            j = ci + 1;
            if (j > count - 1 || j == 0) j = 1;
        } else {
            j = ci + 2;
            if (j > count - 1) j = j - count;
            if (j == 0) j = 1;
        }
        emit(j);
        break;
    }
    case 0x46:
    case 0x47:  // plus the lowest note
        if (!check()) break;
        emit(ci);
        if (count < 2 || ci < 1) break;
        emit(0);
        break;
    case 0x48:
    case 0x49:  // plus the highest note
        if (!check()) break;
        emit(ci);
        if (count < 2 || ci >= count - 1) break;
        emit(count - 1);
        break;
    case 0x4a:  // the whole chord
        if (!check()) break;
        for (int k = 0; k < count; ++k) emit(k);
        break;
    case 0x4b:  // the chord without its lowest note (unless it is the only one)
        if (!check()) break;
        for (int k = count > 1 ? 1 : 0; k < count; ++k) emit(k);
        break;
    case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d:
    case 0x5e:  // chord note cmd - 0x54
        if (cmd - 0x54 >= count) break;
        if (!check()) break;
        emit(cmd - 0x54);
        break;
    case 0x5f:  // the highest note
        if (!check()) break;
        emit(count - 1);
        break;
    default:  // 0x3b, 0x4c..0x53 and out of range: nothing
        break;
    }
}

// ---- FUN_100485d0(frames, songPos, ev) -------------------------------------------------------------------------
int Arp::next(int frames, double songPos, uint8_t ev[4], bool* wrote) {
    if (wrote) *wrote = false;
    if (s_.pendingPhrase) {
        applyPhrase(0, true);
        s_.pendingPhrase = 0;
    }
    s_.songPos = songPos;
    auto pop = [&]() {
        std::memcpy(ev, s_.out[s_.outHead], 4);
        s_.outHead = ring(s_.outHead + 1);
        s_.outCount += -1;
        if (wrote) *wrote = true;
    };
    if (s_.outCount != 0) {
        pop();
        return 0;
    }
    if (!s_.running) {
        s_.songPos = double(frames) * s_.beatsPerSample + songPos;
        return frames;
    }
    if (s_.releasePending) {
        noteListCompact();
        s_.releasePending = 0;
        if (s_.noteCount == 0) s_.stopRequest = 1;
    }
    int n;
    if (s_.stopRequest) {
        s_.running = 0;
        s_.stopRequest = 0;
        if (s_.offCount == 0) return frames;  // returns without advancing songPos
        do {
            pushEvent(0x80, s_.offNote[s_.offHead], 0, 0);
            s_.offCount += -1;
            s_.offHead = ring(s_.offHead + 1);
        } while (s_.offCount != 0);
        pushEvent(0xb0, 0x0b, 0x7f, 0);
        n = 0;
    } else {
        const double x = (s_.nextTime - s_.songPos) * s_.samplesPerBeat;
        // fistp qword (chop), then the low dword compared as int
        int64_t q = std::numeric_limits<int64_t>::min();
        if (x > -9.2233720368547758e18 && x < 9.2233720368547758e18) q = int64_t(x);
        const int lo = int(uint32_t(uint64_t(q)));
        if (lo >= frames) {
            s_.songPos = double(frames) * s_.beatsPerSample + s_.songPos;
            return frames;
        }
        n = lo > 0 ? lo : 0;
        if (s_.nextIsNoteOff == 0) {
            step();
        } else if (s_.nextIsNoteOff == 1) {
            pushEvent(0x80, s_.offNote[s_.offHead], 0, 0);
            s_.offCount += -1;
            s_.offHead = ring(s_.offHead + 1);
        }
        s_.nextTime = nextActionTime(s_.nextTime);
    }
    if (s_.outCount != 0) pop();
    s_.songPos = double(n) * s_.beatsPerSample + s_.songPos;
    return n;
}

void Arp::stateBytes(uint8_t out[sizeof(ArpState)]) const { std::memcpy(out, &s_, sizeof s_); }

}  // namespace hs2
