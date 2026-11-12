// HS2 MIDI Player: a Schwung tool for Ableton Move. Plays Standard MIDI Files on the Hypersonic 2 engine's 16 parts
// (part N = MIDI channel N), and plays MIDI from a USB device on Move's USB-A port the same way. Program changes and
// bank select load patches; with the GM switch on, the engine's General MIDI mapping picks them (banks 78 / 79).
//
// Threads: the audio thread (render_block) runs the sequencer and the engine; a worker thread scans the content,
// reads MIDI files, prepares the patches a file asks for and loads patches chosen by hand. The UI thread calls
// get_param / set_param. `mu` guards the engine and everything the audio thread reads.
//
// Patch loading: every program change in a file is known when it opens, so the worker parses those patches ahead
// ("prepared"); a program change during playback then switches the part inside the same audio block. Anything not
// prepared (USB MIDI, a file that is still preparing) loads on the worker and applies a few blocks later.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "PatchLibrary.h"
#include "hs2/bank.h"
#include "hs2/engine.h"
#include "hs2/patch.h"
#include "hs2/synth.h"
#include "plugin_api_v1.h"
#include "smf.h"

namespace fs = std::filesystem;

namespace {

const host_api_v1_t* g_host = nullptr;

void logf(const char* fmt, ...) {
    if (g_host == nullptr || g_host->log == nullptr) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[540];
    std::snprintf(line, sizeof(line), "hs2mp: %s", buf);
    g_host->log(line);
}

constexpr int kParts = 16;
constexpr int kPP = hs2::Engine::kParamsPerPart;
constexpr int kNoRequest = 0x7f7f;  // the engine's GM "no program" request (banks 64 / 126)

// ------------------------------------------------------------------------------------------------ text helpers

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Patch names are Latin-1; the display font and the JSON to the UI want ASCII.
std::string ascii(const std::string& latin1) {
    static const char* const kHi[96] = {
        " ", "!", "c", "L", "o", "Y", "|", "S", "\"", "(c)", "a", "<<", "-", "-", "(r)", "-",
        "o", "+-", "2", "3", "'", "u", "P", ".", ",", "1", "o", ">>", "1/4", "1/2", "3/4", "?",
        "A", "A", "A", "A", "Ae", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "Oe", "x", "O", "U", "U", "U", "Ue", "Y", "Th", "ss",
        "a", "a", "a", "a", "ae", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "oe", "/", "o", "u", "u", "u", "ue", "y", "th", "y"};
    std::string out;
    for (unsigned char c : latin1) {
        if (c == 0) break;
        if (c >= 32 && c < 127) out += char(c);
        else if (c >= 160) out += kHi[c - 160];
        else out += ' ';
    }
    return trim(out);
}

std::string jstr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 32 || c >= 127) out += ' ';
        else out += char(c);
    }
    return out + "\"";
}

std::string fnum(double x, int prec = 3) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", prec, x);
    return b;
}

int writeOut(char* buf, int len, const std::string& s) {
    if (!buf || len <= 0) return -1;
    const int n = int(std::min<size_t>(s.size(), size_t(len - 1)));
    std::memcpy(buf, s.data(), size_t(n));
    buf[n] = 0;
    return n;
}

// "key":value (number or "string") from a flat JSON object: enough for the settings file this module writes.
bool jsonNum(const std::string& j, const std::string& key, double& out) {
    const auto k = j.find("\"" + key + "\":");
    if (k == std::string::npos) return false;
    out = std::atof(j.c_str() + k + key.size() + 3);
    return true;
}
bool jsonStr(const std::string& j, const std::string& key, std::string& out) {
    const auto k = j.find("\"" + key + "\":\"");
    if (k == std::string::npos) return false;
    std::string v;
    for (size_t i = k + key.size() + 4; i < j.size() && j[i] != '"'; ++i) {
        if (j[i] == '\\' && i + 1 < j.size()) ++i;
        v += j[i];
    }
    out = v;
    return true;
}

// ------------------------------------------------------------------------------------------------ program requests

// Engine::processEvents' request for a program change (engine.cpp case 0xc0), for reading a file ahead.
int requestFor(bool gm, int ch, int bankSel, int prog) {
    if (!gm) return bankSel * 0x100 + prog;
    const int b = bankSel >> 7;
    const bool drums = ch == 9 ? b != 0x79 : (b == 0x78 || b == 0xff);
    if (b == 0x40 || b == 0x7e) return kNoRequest;
    return prog + (drums ? 0x4f00 : 0x4e00);
}

// A request's bank (CC0 * 128 + CC32, or the GM banks 78 / 79) as a content bank number: CC32 alone or CC0 alone.
int contentBank(int bankSel) {
    if (bankSel < 128) return bankSel;
    const int msb = bankSel >> 7, lsb = bankSel & 0x7f;
    return lsb == 0 ? msb : lsb;
}

int keyOf(int bank, int prog) { return bank << 8 | prog; }

// ------------------------------------------------------------------------------------------------ state

struct Settings {
    int tempo = 0;     // 0 the file's tempo, 1 follow Move (tempo and transport)
    int gm = 1;        // program changes through the engine's GM mapping
    int usb = 1;       // play MIDI from USB devices
    int loop = 0;
    int preload = 1;   // decode the parts' samples on the worker after loading
    int ringOut = 1;
    float master = -1; // engine master volume (-1 = the engine's default)
    std::string lastDir;
};

struct PartInfo {
    std::string name = "empty", bankName;
    int bank = -1, prog = -1;
    bool mute = false, solo = false, lock = false;
    bool used = false;   // the open file plays notes on this channel
    int request = -1;    // the content key (bank << 8 | prog) the part was asked for last
    std::string missing; // the last request that has no patch in the content
};

struct Prepared {
    std::shared_ptr<const hs2::Patch> patch;  // nullptr: no such patch (missing)
    int bank = -1, prog = -1;                 // the patch actually used (GM fallbacks change it)
    std::string name, bankName;
};

struct SongInfo {
    std::shared_ptr<const smf::Song> song;
    std::string path, name;
    std::array<int, kParts> firstReq{};       // the content key each used channel starts with (-1 unused)
    std::array<int, kParts> firstBankSel{}, firstProg{};  // the bank select and program that give it
    std::vector<int> requests;                // every content key the file asks for, first use first
    double loopQn = 0;                        // the end rounded up to a whole bar (sync mode loops on bars)
};

struct LoadJob {
    int bank = 0, prog = 0;
    bool byHand = false;  // chosen in the UI: the part is locked to it
    uint64_t gen = 0;
};

struct Command {
    int part = 0;
    std::shared_ptr<const hs2::Patch> patch;
    Prepared prep;
    bool byHand = false;
    uint64_t gen = 0;
    int key = -1;
};

struct BankRow {
    int number;
    std::string name;
    std::vector<std::pair<int, std::string>> patches;  // index, name
};

struct Instance {
    std::string moduleDir;
    fs::path contentRoot;

    std::mutex mu;  // everything below up to the worker section
    std::unique_ptr<hs2::Content> content;
    std::unique_ptr<hs2::Engine> engine;
    std::shared_ptr<const hs2port::PatchLibrary> lib;
    std::vector<BankRow> banks;
    int scanState = 0;  // 0 scanning, 1 ready, -1 no content
    Settings set;
    std::array<PartInfo, kParts> parts;
    std::map<int, Prepared> prepared;          // content key -> patch
    std::vector<Command> cmds;                 // loads the worker finished
    std::vector<hs2::MidiEvent> live;          // USB MIDI for the next block
    std::vector<hs2::MidiEvent> events;        // the block's events
    std::vector<float> buf;
    // the open file and the transport (audio thread)
    SongInfo song;
    std::string songError;
    bool playing = false;
    bool startWhenPrepared = false;
    double posSec = 0, posQn = 0, posTick = 0;
    size_t nextEv = 0;
    bool atEnd = false;
    std::vector<hs2::MidiEvent> chaseEvents;   // controller state to send at the next block (seek, start)
    bool moveRunning = false;
    double anchor = 0;                         // sync: file quarter note = Move beat - anchor
    bool syncWaiting = false;
    int preparing = 0;                         // patches of the open file still being prepared
    int prepTotal = 0;
    std::array<int, kParts> noteCount{};       // note-ons per part (for the pads)
    // meters
    double cpuAvg = 0, cpuPeak = 0;
    int cpuPeakHold = 0;

    // worker
    std::thread worker;
    std::mutex qmu;
    std::condition_variable cv;
    bool quit = false;
    bool scanRequested = true;
    std::optional<std::string> openRequest;
    uint64_t fileGen = 0;                      // guarded by qmu: a newer file stops the preparing of an older one
    std::array<std::optional<LoadJob>, kParts> jobs;
    std::array<uint64_t, kParts> loadGen{};    // guarded by qmu
    std::string status = "Scanning content...";
    size_t preloadBudget = size_t(256) << 20;
    bool sweepDue = false;                     // worker only
    std::array<std::shared_ptr<const hs2::Patch>, kParts> preloadTodo;  // worker only: decode these parts' samples
    std::array<bool, kParts> preloadWanted{};  // guarded by qmu: set when a part's patch changed

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(qmu);
        status = s;
    }
    std::string getStatus() {
        std::lock_guard<std::mutex> lk(qmu);
        return status;
    }
    void postLoad(int part, LoadJob j) {  // qmu not held
        {
            std::lock_guard<std::mutex> lk(qmu);
            j.gen = ++loadGen[size_t(part)];
            jobs[size_t(part)] = j;
        }
        cv.notify_one();
    }

    void workerLoop();
    void scan();
    void openFile(const std::string& path, uint64_t gen);
    bool prepare(int key, Prepared& out);  // worker: parse the patch for a content key (with GM fallbacks)
    void runLoad(int part, LoadJob job);
    void runPreloads();
    bool releaseUnusedSamples();
    void saveSettings();                   // mu held
    void loadSettings();
    void applySettings();                  // mu held

    // audio thread (mu held)
    void applyCommands();
    void request(int part, int bankSel, int prog);  // an engine program request
    bool applyKey(int part, int key, bool byHand);  // prepared: now; else on the worker. false if neither
    void play();
    void stop();
    void seekTick(double tick);
    void chase();                                   // controller and program state at posTick, for the next block
    void notesOff();
    void updateMutes();
    void render(int16_t* out, int frames);
};

// ------------------------------------------------------------------------------------------------ settings

void Instance::loadSettings() {
    std::ifstream f(fs::path(moduleDir) / "settings.json");
    if (!f) return;
    const std::string j((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    double v;
    if (jsonNum(j, "tempo", v)) set.tempo = v > 0.5;
    if (jsonNum(j, "gm", v)) set.gm = v > 0.5;
    if (jsonNum(j, "usb", v)) set.usb = v > 0.5;
    if (jsonNum(j, "loop", v)) set.loop = v > 0.5;
    if (jsonNum(j, "preload", v)) set.preload = v > 0.5;
    if (jsonNum(j, "ringout", v)) set.ringOut = v > 0.5;
    if (jsonNum(j, "master", v)) set.master = float(std::clamp(v, -1.0, 1.0));
    jsonStr(j, "lastdir", set.lastDir);
}

void Instance::saveSettings() {
    std::string j = "{\"tempo\":" + std::to_string(set.tempo) + ",\"gm\":" + std::to_string(set.gm) +
                    ",\"usb\":" + std::to_string(set.usb) + ",\"loop\":" + std::to_string(set.loop) +
                    ",\"preload\":" + std::to_string(set.preload) + ",\"ringout\":" + std::to_string(set.ringOut) +
                    ",\"master\":" + fnum(set.master, 4) + ",\"lastdir\":" + jstr(set.lastDir) + "}\n";
    std::ofstream f(fs::path(moduleDir) / "settings.json", std::ios::trunc);
    if (f) f << j;
}

void Instance::applySettings() {
    if (!engine) return;
    engine->setGmMode(set.gm != 0);
    engine->setRingOut(set.ringOut != 0);
    if (set.master >= 0) engine->setParameter(0, set.master);
}

// ------------------------------------------------------------------------------------------------ worker

void Instance::scan() {
    const auto t0 = std::chrono::steady_clock::now();
    fs::path root = contentRoot;
    std::error_code ec;
    auto hasPatches = [&](const fs::path& r) { return fs::is_directory(r / "Patches", ec); };
    auto inside = [&](fs::path r) {  // the content folder copied with its own name inside
        if (hasPatches(r)) return r;
        for (const auto& e : fs::directory_iterator(r, ec))
            if (e.is_directory() && hasPatches(e.path())) return e.path();
        return r;
    };
    root = inside(root);
    if (!hasPatches(root)) {  // the Hypersonic 2 sound generator's content folder
        const fs::path sg = fs::path(moduleDir).parent_path().parent_path() / "sound_generators" / "hypersonic2" / "content";
        if (hasPatches(inside(sg))) root = inside(sg);
    }
    if (!hasPatches(root)) {
        std::lock_guard<std::mutex> lk(mu);
        scanState = -1;
        setStatus("Hypersonic 2 content not found. Copy the content folder to " + contentRoot.string() +
                  ", or install the Hypersonic 2 module with its content.");
        logf("content not found under %s", contentRoot.c_str());
        return;
    }
    auto content_ = std::make_unique<hs2::Content>(root);
    auto engine_ = std::make_unique<hs2::Engine>(*content_, float(g_host ? g_host->sample_rate : 44100));
    auto lib_ = std::make_shared<hs2port::PatchLibrary>(hs2port::PatchLibrary::scan(root));
    std::vector<BankRow> banks_;
    size_t count = 0;
    for (const auto& b : lib_->banks) {
        if (b.combi) continue;
        BankRow row{b.number, ascii(b.name), {}};
        for (int pi : b.patches) {
            const auto& p = lib_->patches[size_t(pi)];
            if (p.copyProtected) continue;
            std::string n = ascii(hs2port::displayName(p.name));
            if (n.empty()) n = "Patch " + std::to_string(p.index + 1);
            row.patches.push_back({p.index, n});
        }
        count += row.patches.size();
        if (!row.patches.empty()) banks_.push_back(std::move(row));
    }
    for (const auto& err : lib_->errors) logf("library: %s", err.c_str());
    {
        std::lock_guard<std::mutex> lk(mu);
        cmds.clear();
        prepared.clear();
        engine.reset();  // before the content it reads from
        content = std::move(content_);
        engine = std::move(engine_);
        for (auto& p : parts) {
            const bool used = p.used;
            p = PartInfo{};
            p.used = used;
        }
        applySettings();
        lib = lib_;
        banks = std::move(banks_);
        scanState = 1;
        playing = false;
    }
    for (auto& t : preloadTodo) t.reset();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logf("content %s: %zu patches in %zu banks (%.0f ms)", root.c_str(), count, banks.size(), ms);
    setStatus("Ready. " + std::to_string(count) + " patches.");
}

bool Instance::prepare(int key, Prepared& out) {
    std::shared_ptr<const hs2port::PatchLibrary> L;
    hs2::Content* C;
    {
        std::lock_guard<std::mutex> lk(mu);
        L = lib;
        C = content.get();
    }
    if (!L || !C) return false;
    const int bank = key >> 8, prog = key & 0xff;
    std::vector<std::pair<int, int>> tries = {{bank, prog}};
    if (bank == 79 && prog != 0) tries.push_back({79, 0});  // GM drums: a kit that is not there plays the Standard kit
    if (bank == 78) {                                       // GM: the instrument family's first, then the piano
        if ((prog & ~7) != prog) tries.push_back({78, prog & ~7});
        if (prog != 0) tries.push_back({78, 0});
    }
    for (const auto& [b, p] : tries) {
        const int li = L->find(b, p);
        if (li < 0 || L->patches[size_t(li)].copyProtected) continue;
        try {
            const auto path = C->patchBankPath(b);
            if (path.empty()) continue;
            auto mb = hs2::MilanBank::open(path);
            if (mb->kind != 1 || size_t(p) >= mb->entries.size() || mb->entries[size_t(p)].slot1.size == 0) continue;
            const auto data = mb->read(mb->entries[size_t(p)].slot1);
            if (hs2port::isCopyProtectedPatch(data.data(), data.size())) continue;
            auto patch = std::make_shared<hs2::Patch>(hs2::Patch::parse(std::vector<uint8_t>(data.begin(), data.end())));
            {
                // a throwaway player opens the patch's sample banks and wavetables into the content's caches, so the
                // engine's load on the audio thread finds them there
                hs2::PatchPlayer tmp(*C, float(g_host ? g_host->sample_rate : 44100));
                tmp.load(*patch);
            }
            out.patch = patch;
            out.bank = b;
            out.prog = p;
            out.name = ascii(hs2port::displayName(L->patches[size_t(li)].name));
            if (out.name.empty()) out.name = ascii(hs2port::displayName(patch->name));
            if (out.name.empty()) out.name = "Patch " + std::to_string(p + 1);
            if (const auto* bi = L->bank(b)) out.bankName = ascii(bi->name);
            return true;
        } catch (const std::exception& e) {
            logf("patch %d of bank %d: %s", p + 1, b, e.what());
        }
    }
    out = Prepared{};
    out.bank = bank;
    out.prog = prog;
    out.name = "missing " + std::to_string(bank) + ":" + std::to_string(prog + 1);
    return true;  // known to be missing
}

void Instance::openFile(const std::string& path, uint64_t gen) {
    SongInfo si;
    si.path = path;
    si.name = fs::path(path).filename().string();
    std::string err;
    try {
        si.song = std::make_shared<const smf::Song>(smf::load(path));
    } catch (const std::exception& e) {
        err = e.what();
    }
    bool gm;
    {
        std::lock_guard<std::mutex> lk(mu);
        gm = set.gm != 0;
    }
    if (si.song) {
        const auto& s = *si.song;
        for (const auto& w : s.warnings) logf("%s: %s", si.name.c_str(), w.c_str());
        // Read ahead: the patch every channel starts with and every patch the file asks for.
        std::array<int, kParts> bankSel{}, cur{}, pcBankSel{}, pcProg{};
        std::array<bool, kParts> started{};
        for (int c = 0; c < kParts; ++c) cur[size_t(c)] = requestFor(gm, c, 0, 0);
        si.firstReq.fill(-1);
        std::set<int> seen;
        auto use = [&](int req) {
            if (req == kNoRequest) return;
            const int key = keyOf(contentBank(req >> 8), req & 0xff);
            if (seen.insert(key).second) si.requests.push_back(key);
        };
        for (const auto& e : s.events) {
            if (e.len == 0) continue;
            const int ch = e.m[0] & 0x0f, t = e.m[0] & 0xf0;
            if (t == 0xb0 && e.m[1] == 0) bankSel[size_t(ch)] = e.m[2] * 0x80 + (bankSel[size_t(ch)] & 0x7f);
            else if (t == 0xb0 && e.m[1] == 32) bankSel[size_t(ch)] = (bankSel[size_t(ch)] & 0x3f80) + e.m[2];
            else if (t == 0xc0) {
                pcBankSel[size_t(ch)] = bankSel[size_t(ch)];
                pcProg[size_t(ch)] = e.m[1];
                cur[size_t(ch)] = requestFor(gm, ch, bankSel[size_t(ch)], e.m[1]);
                if (started[size_t(ch)]) use(cur[size_t(ch)]);
            } else if (t == 0x90 && e.m[2] > 0 && !started[size_t(ch)]) {
                started[size_t(ch)] = true;
                si.firstBankSel[size_t(ch)] = pcBankSel[size_t(ch)];
                si.firstProg[size_t(ch)] = pcProg[size_t(ch)];
                const int req = cur[size_t(ch)];
                if (req != kNoRequest) {
                    si.firstReq[size_t(ch)] = keyOf(contentBank(req >> 8), req & 0xff);
                    use(req);
                }
            }
        }
        // the requests every channel starts with go first
        std::vector<int> order;
        for (int c = 0; c < kParts; ++c)
            if (si.firstReq[size_t(c)] >= 0 && std::find(order.begin(), order.end(), si.firstReq[size_t(c)]) == order.end())
                order.push_back(si.firstReq[size_t(c)]);
        for (int k : si.requests)
            if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
        si.requests = order;
        // sync mode loops on whole bars
        const smf::Sig& g = s.sigAt(s.bars.back());
        const double barLen = double(s.ppq) * 4.0 * g.num / g.den;
        double end = double(s.bars.back());
        if (end < double(s.endTick)) end += barLen;
        si.loopQn = std::max(s.qnOf(end), 1.0);
    }
    std::vector<int> todo;
    {
        std::lock_guard<std::mutex> lk(mu);
        {
            std::lock_guard<std::mutex> q(qmu);
            if (gen != fileGen) return;  // another file was chosen meanwhile
        }
        if (engine && playing) {
            playing = false;
            notesOff();
        }
        if (!si.song) {
            songError = err;
            song = SongInfo{};
            for (auto& p : parts) p.used = false;
        } else {
            songError.clear();
            song = si;
            for (int c = 0; c < kParts; ++c) parts[size_t(c)].used = si.firstReq[size_t(c)] >= 0;
            for (int k : si.requests)
                if (!prepared.count(k)) todo.push_back(k);
            posSec = posQn = posTick = 0;
            nextEv = 0;
            atEnd = false;
            moveRunning = false;
            syncWaiting = false;
            preparing = int(todo.size());
            prepTotal = preparing;
            startWhenPrepared = false;
        }
    }
    if (!si.song) {
        setStatus("Could not open " + si.name + ": " + err);
        logf("%s: %s", path.c_str(), err.c_str());
        return;
    }
    logf("%s: format %d, %d tracks, %zu events, %.1f s, %zu bars, %zu patches (%zu to prepare)", si.name.c_str(),
         si.song->format, si.song->tracks, si.song->events.size(), si.song->endSec, si.song->bars.size(),
         si.requests.size(), todo.size());
    setStatus(si.name);
    for (size_t i = 0; i < todo.size(); ++i) {
        {
            std::lock_guard<std::mutex> q(qmu);
            if (gen != fileGen || quit || scanRequested) return;
        }
        Prepared pr;
        if (!prepare(todo[i], pr)) return;
        std::lock_guard<std::mutex> lk(mu);
        prepared[todo[i]] = pr;
        preparing = int(todo.size() - i - 1);
        if (!pr.patch) logf("%s: %s", si.name.c_str(), pr.name.c_str());
    }
    std::lock_guard<std::mutex> lk(mu);
    preparing = 0;
    // the patches the channels start with, before the first note
    if (!playing) chase();
}

void Instance::runLoad(int part, LoadJob job) {
    Prepared pr;
    const int key = keyOf(job.bank, job.prog);
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = prepared.find(key);
        if (it != prepared.end()) pr = it->second;
    }
    if (!pr.patch && pr.name.empty() && !prepare(key, pr)) return;
    std::lock_guard<std::mutex> lk(mu);
    {
        std::lock_guard<std::mutex> q(qmu);
        if (job.gen != loadGen[size_t(part)]) return;  // a newer request for this part
    }
    prepared[key] = pr;
    Command c;
    c.part = part;
    c.patch = pr.patch;
    c.prep = pr;
    c.byHand = job.byHand;
    c.gen = job.gen;
    c.key = key;
    cmds.push_back(std::move(c));
}

void Instance::runPreloads() {
    for (int p = 0; p < kParts; ++p) {
        std::shared_ptr<const hs2::Patch> patch;
        {
            std::lock_guard<std::mutex> q(qmu);
            if (!preloadWanted[size_t(p)]) continue;
            preloadWanted[size_t(p)] = false;
        }
        hs2::Content* C;
        {
            std::lock_guard<std::mutex> lk(mu);
            const int key = parts[size_t(p)].request;
            auto it = prepared.find(key);
            if (it == prepared.end() || !it->second.patch || !set.preload) continue;
            patch = it->second.patch;
            C = content.get();
        }
        if (!C) continue;
        hs2::PatchPlayer tmp(*C, float(g_host ? g_host->sample_rate : 44100));
        tmp.load(*patch);
        bool stopped = false;
        const size_t got = tmp.preloadSamples([&] {
            {
                std::lock_guard<std::mutex> q(qmu);
                if (quit || scanRequested || openRequest) return !(stopped = true);
                for (const auto& j : jobs)
                    if (j) return !(stopped = true);
            }
            return C->decodedSampleBytes() < preloadBudget || !(stopped = true);
        });
        if (got > (1u << 20))
            logf("part %d: %.1f MB of samples ready%s (%.1f MB decoded in all)", p + 1, double(got) / 1048576.0,
                 stopped ? ", stopped early" : "", double(C->decodedSampleBytes()) / 1048576.0);
        if (stopped) return;
    }
}

// Frees the decoded samples no player can read any more (see the Hypersonic 2 module: releaseUnusedSamples).
bool Instance::releaseUnusedSamples() {
    std::vector<std::unique_ptr<hs2::SampleData>> dead;
    size_t freed = 0, held = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!content || !engine) return true;
        if (!cmds.empty()) return false;
        hs2::PatchPlayer::SampleRefs refs;
        engine->collectSampleRefs(refs);
        std::sort(refs.zones.begin(), refs.zones.end());
        const std::unordered_set<const hs2::SampleData*> playingNow(refs.voices.begin(), refs.voices.end());
        freed = content->releaseSamples(
            [&](const hs2::SampleBank* b, uint32_t i, const hs2::SampleData* sd) {
                return playingNow.count(sd) || std::binary_search(refs.zones.begin(), refs.zones.end(),
                                                                  std::pair<const hs2::SampleBank*, uint32_t>{b, i});
            },
            dead);
        if (freed) held = content->decodedSampleBytes();
    }
    dead.clear();
    if (freed) logf("samples: released %.1f MB, %.1f MB still decoded", double(freed) / 1048576.0, double(held) / 1048576.0);
    return true;
}

void Instance::workerLoop() {
    for (;;) {
        bool doScan = false;
        std::optional<std::string> open;
        uint64_t gen = 0;
        int part = -1;
        LoadJob lj;
        {
            std::unique_lock<std::mutex> lk(qmu);
            const auto queued = [this] {
                if (quit || scanRequested || openRequest) return true;
                for (const auto& j : jobs)
                    if (j) return true;
                return false;
            };
            bool preloadsDue = false;
            for (bool w : preloadWanted) preloadsDue = preloadsDue || w;
            const bool woke = preloadsDue ? queued()
                                          : cv.wait_for(lk, std::chrono::milliseconds(sweepDue ? 100 : 2000), queued);
            if (quit) return;
            if (!woke) {
                lk.unlock();
                if (preloadsDue) runPreloads();
                else if (releaseUnusedSamples()) sweepDue = false;
                continue;
            }
            if (scanRequested) {
                scanRequested = false;
                doScan = true;
            } else if (openRequest) {
                open = std::move(openRequest);
                openRequest.reset();
                gen = fileGen;
            } else {
                for (int p = 0; p < kParts && part < 0; ++p)
                    if (jobs[size_t(p)]) {
                        part = p;
                        lj = *jobs[size_t(p)];
                        jobs[size_t(p)].reset();
                    }
            }
        }
        if (doScan) {
            scan();
            std::string reopen;
            {
                std::lock_guard<std::mutex> lk(mu);
                reopen = song.path;
            }
            if (!reopen.empty()) {
                std::lock_guard<std::mutex> q(qmu);
                openRequest = reopen;
                ++fileGen;
            }
        } else if (open) {
            openFile(*open, gen);
            sweepDue = true;
        } else if (part >= 0) {
            runLoad(part, lj);
            sweepDue = true;
        }
    }
}

// ------------------------------------------------------------------------------------------------ audio thread

void Instance::notesOff() {
    for (int c = 0; c < kParts; ++c) {
        hs2::MidiEvent e;
        e.data[0] = uint8_t(0xb0 | c);
        e.data[1] = 64;  // sustain off
        chaseEvents.push_back(e);
        e.data[1] = 123;  // all notes off
        chaseEvents.push_back(e);
    }
}

void Instance::updateMutes() {
    if (!engine) return;
    bool anySolo = false;
    for (const auto& p : parts) anySolo = anySolo || p.solo;
    for (int p = 0; p < kParts; ++p) {
        const bool m = parts[size_t(p)].mute || (anySolo && !parts[size_t(p)].solo);
        if ((engine->getParameter(p * kPP + 1) > 0.5f) != m) engine->setParameter(p * kPP + 1, m ? 1.f : 0.f);
    }
}

bool Instance::applyKey(int p, int key, bool byHand) {
    PartInfo& pi = parts[size_t(p)];
    auto it = prepared.find(key);
    if (it != prepared.end()) {
        pi.request = key;
        if (!it->second.patch) {
            pi.missing = it->second.name;
            return true;
        }
        engine->loadPatch(p, *it->second.patch, it->second.bank);
        pi.name = it->second.name;
        pi.bankName = it->second.bankName;
        pi.bank = it->second.bank;
        pi.prog = it->second.prog;
        pi.missing.clear();
        if (byHand) pi.lock = true;
        updateMutes();  // a patch load resets nothing of the mixer, but a fresh part starts unmuted
        {
            std::lock_guard<std::mutex> q(qmu);
            preloadWanted[size_t(p)] = true;
        }
        cv.notify_one();
        return true;
    }
    pi.request = key;
    LoadJob j;
    j.bank = key >> 8;
    j.prog = key & 0xff;
    j.byHand = byHand;
    postLoad(p, j);
    return false;
}

void Instance::request(int p, int bankSel, int prog) {
    if (p < 0 || p >= kParts || parts[size_t(p)].lock) return;
    const int req = bankSel * 0x100 + prog;
    if (req == kNoRequest) return;
    applyKey(p, keyOf(contentBank(bankSel), prog), false);
}

void Instance::applyCommands() {
    for (auto& c : cmds) {
        std::uint64_t latest;
        {
            std::lock_guard<std::mutex> q(qmu);
            latest = loadGen[size_t(c.part)];
        }
        if (c.gen != latest) continue;
        if (!c.byHand && parts[size_t(c.part)].lock) continue;
        if (!c.byHand && parts[size_t(c.part)].request != c.key) continue;  // the file moved on
        applyKey(c.part, c.key, c.byHand);
    }
    cmds.clear();
}

void Instance::chase() {
    if (!song.song || !engine) return;
    const auto& s = *song.song;
    // Every controller, pitch bend and pressure before the position, in order (RPN sequences need the order), then
    // each used channel's program with the bank select it had then.
    std::array<int, kParts> bankSel{}, pcBank{}, prog;
    prog.fill(-1);
    for (int c = 0; c < kParts; ++c) {
        hs2::MidiEvent e;
        e.data[0] = uint8_t(0xb0 | c);
        e.data[1] = 121;  // reset all controllers
        chaseEvents.push_back(e);
    }
    // Controllers before the position are sent; program changes count up to and including it (one at the very
    // position plays when the file starts there, but the part should show it already).
    std::array<int, kParts> pcBankSel{}, pcSeen{};
    for (size_t i = 0; i < s.events.size(); ++i) {
        const auto& ev = s.events[i];
        if (i >= nextEv && double(ev.tick) > posTick) break;
        if (ev.len == 0) continue;
        const int ch = ev.m[0] & 0x0f, t = ev.m[0] & 0xf0;
        if (t == 0x80 || t == 0x90 || t == 0xa0) continue;
        if (t == 0xb0 && ev.m[1] == 0) pcBankSel[size_t(ch)] = ev.m[2] * 0x80 + (pcBankSel[size_t(ch)] & 0x7f);
        if (t == 0xb0 && ev.m[1] == 32) pcBankSel[size_t(ch)] = (pcBankSel[size_t(ch)] & 0x3f80) + ev.m[2];
        if (t == 0xc0) {
            prog[size_t(ch)] = ev.m[1];
            pcBank[size_t(ch)] = pcBankSel[size_t(ch)];
            pcSeen[size_t(ch)] = 1;
            continue;
        }
        if (i >= nextEv) continue;
        if (t == 0xb0 && ev.m[1] == 0) bankSel[size_t(ch)] = ev.m[2] * 0x80 + (bankSel[size_t(ch)] & 0x7f);
        if (t == 0xb0 && ev.m[1] == 32) bankSel[size_t(ch)] = (bankSel[size_t(ch)] & 0x3f80) + ev.m[2];
        if (t == 0xb0 && (ev.m[1] == 120 || ev.m[1] == 123)) continue;
        hs2::MidiEvent e;
        std::memcpy(e.data, ev.m, 3);
        chaseEvents.push_back(e);
    }
    for (int c = 0; c < kParts; ++c) {
        if (!parts[size_t(c)].used && prog[size_t(c)] < 0) continue;
        // no program change yet: the program the channel's first note plays with
        const int b = pcSeen[size_t(c)] ? pcBank[size_t(c)] : song.firstBankSel[size_t(c)];
        const int pr = pcSeen[size_t(c)] ? prog[size_t(c)] : song.firstProg[size_t(c)];
        auto cc = [&](int n, int v) {
            hs2::MidiEvent e;
            e.data[0] = uint8_t(0xb0 | c);
            e.data[1] = uint8_t(n);
            e.data[2] = uint8_t(v);
            chaseEvents.push_back(e);
        };
        cc(0, b >> 7);
        cc(32, b & 0x7f);
        hs2::MidiEvent e;
        e.data[0] = uint8_t(0xc0 | c);
        e.data[1] = uint8_t(pr);
        chaseEvents.push_back(e);
        // The engine asks only for a program that differs from the channel's last one: a part whose patch was
        // changed by hand or by a later program change gets it directly.
        if (!parts[size_t(c)].lock) {
            const int req = requestFor(set.gm != 0, c, b, pr);
            if (req != kNoRequest) {
                const int key = keyOf(contentBank(req >> 8), req & 0xff);
                if (parts[size_t(c)].request != key) applyKey(c, key, false);
            }
        }
        cc(0, bankSel[size_t(c)] >> 7);
        cc(32, bankSel[size_t(c)] & 0x7f);
    }
}

void Instance::seekTick(double tick) {
    if (!song.song) return;
    const auto& s = *song.song;
    tick = std::clamp(tick, 0.0, double(s.endTick));
    posTick = tick;
    posSec = s.secAt(tick);
    posQn = s.qnOf(tick);
    nextEv = size_t(std::lower_bound(s.events.begin(), s.events.end(), tick,
                                     [](const smf::Event& e, double t) { return double(e.tick) < t; }) -
                    s.events.begin());
    atEnd = false;
    notesOff();
    chase();
    if (set.tempo == 1 && moveRunning) {  // keep following Move from here
        const double beat = g_host && g_host->get_beat_position ? g_host->get_beat_position() : 0.0;
        anchor = beat - posQn;
    }
}

void Instance::play() {
    if (!song.song) return;
    if (preparing > 0) {  // starts when the file's patches are there
        startWhenPrepared = true;
        return;
    }
    if (atEnd || posTick >= double(song.song->endTick)) seekTick(0);
    playing = true;
    startWhenPrepared = false;
}

void Instance::stop() {
    playing = false;
    startWhenPrepared = false;
    syncWaiting = false;
    notesOff();
}

void Instance::render(int16_t* out, int frames) {
    const auto t0 = std::chrono::steady_clock::now();
    const double sr = g_host ? double(g_host->sample_rate) : 44100.0;
    if (!cmds.empty()) applyCommands();
    if (startWhenPrepared && preparing == 0) play();
    events.clear();
    events.insert(events.end(), chaseEvents.begin(), chaseEvents.end());
    chaseEvents.clear();
    events.insert(events.end(), live.begin(), live.end());
    live.clear();

    // Move's clock
    double bpm = g_host && g_host->get_bpm ? double(g_host->get_bpm()) : 120.0;
    if (!(bpm >= 20.0 && bpm <= 999.0)) bpm = 120.0;
    const int clock = g_host && g_host->get_clock_status ? g_host->get_clock_status() : MOVE_CLOCK_STATUS_UNAVAILABLE;
    const double beat = g_host && g_host->get_beat_position ? g_host->get_beat_position() : -1.0;
    const bool running = beat >= 0.0 && clock != MOVE_CLOCK_STATUS_STOPPED;

    hs2::HostTransport tr;
    const smf::Song* s = song.song.get();
    if (s && set.tempo == 1) {
        // Follow Move: the file's quarter notes run on Move's beats; Move's Play and Stop start and stop it.
        if (running && !moveRunning && preparing == 0) {
            moveRunning = true;
            if (atEnd || posTick >= double(s->endTick)) seekTick(0);
            // start on a bar line of Move's: now if Move is on one, else at the next
            const double b4 = beat / 4.0;
            anchor = (b4 - std::floor(b4) < 1e-3 ? beat : std::ceil(b4) * 4.0) - posQn;
            playing = true;
        } else if (!running && moveRunning) {
            moveRunning = false;
            if (playing) stop();
        }
        if (playing && running) {
            const double q0 = beat - anchor, q1 = q0 + double(frames) / sr * bpm / 60.0;
            syncWaiting = q0 < posQn - 1e-9;
            if (q1 > posQn) {
                const double from = std::max(q0, posQn);
                while (nextEv < s->events.size() && s->events[nextEv].qn < q1) {
                    const auto& ev = s->events[nextEv++];
                    if (ev.len == 0) continue;
                    hs2::MidiEvent e;
                    std::memcpy(e.data, ev.m, 3);
                    e.offset = std::clamp(int((ev.qn - q0) / (q1 - q0) * frames), 0, frames - 1);
                    (void)from;
                    events.push_back(e);
                }
                posQn = q1;
                posTick = s->tickOfQn(posQn);
                posSec = s->secAt(posTick);
                if (posQn >= song.loopQn && nextEv >= s->events.size()) {
                    if (set.loop) {
                        // the file starts again where its last bar ends, on Move's bar grid
                        const double next = anchor + song.loopQn;
                        seekTick(0);
                        anchor = next;
                    } else {
                        playing = false;
                        atEnd = true;
                        notesOff();
                    }
                }
            }
        }
        tr.bpm = bpm;
        tr.playing = running;
        tr.ppq = beat >= 0 ? beat : 0.0;
        tr.barStart = std::floor(tr.ppq / 4.0) * 4.0;
    } else if (s) {
        // The file's own tempo map.
        if (playing) {
            const double end = posSec + double(frames) / sr;
            while (nextEv < s->events.size() && s->events[nextEv].sec < end) {
                const auto& ev = s->events[nextEv++];
                if (ev.len == 0) continue;
                hs2::MidiEvent e;
                std::memcpy(e.data, ev.m, 3);
                e.offset = std::clamp(int((ev.sec - posSec) * sr), 0, frames - 1);
                events.push_back(e);
            }
            posSec = end;
            posTick = s->tickAtSec(posSec);
            posQn = s->qnOf(posTick);
            if (nextEv >= s->events.size() && posSec >= s->endSec) {
                if (set.loop) {
                    seekTick(0);
                } else {
                    playing = false;
                    atEnd = true;
                    notesOff();
                }
            }
        }
        tr.bpm = s->bpmAt(uint32_t(posTick));
        tr.playing = playing;
        tr.ppq = posQn;
        int bar;
        double bt;
        s->barBeat(posTick, bar, bt);
        tr.barStart = s->qnOf(double(s->barTick(bar)));
        const smf::Sig& g = s->sigAt(uint32_t(posTick));
        tr.sigNum = g.num;
        tr.sigDen = g.den;
    } else {
        tr.bpm = bpm;
        tr.playing = running;
        tr.ppq = beat >= 0 ? beat : 0.0;
        tr.barStart = std::floor(tr.ppq / 4.0) * 4.0;
    }

    // note-ons per part, for the pads
    for (const auto& e : events)
        if ((e.data[0] & 0xf0) == 0x90 && e.data[2] > 0) ++noteCount[size_t(e.data[0] & 0x0f)];

    // Render in segments that end after each program change, so the notes after it play the new patch.
    std::stable_sort(events.begin(), events.end(), [](const hs2::MidiEvent& a, const hs2::MidiEvent& b) {
        return a.offset < b.offset;
    });
    int start = 0;
    size_t ei = 0;
    while (start < frames) {
        int endF = frames;
        size_t ej = ei;
        while (ej < events.size()) {
            const bool pc = (events[ej].data[0] & 0xf0) == 0xc0;
            ++ej;
            if (pc) {
                endF = std::max(start + 1, std::min(frames, events[ej - 1].offset + 1));
                break;
            }
        }
        if (endF == frames) ej = events.size();
        // events past this segment's end stay for the next one
        size_t take = ei;
        while (take < ej && events[take].offset < endF) ++take;
        if (take < ej) ej = take;
        for (int pos = start; pos < endF;) {
            const int n = std::min(endF - pos, int(buf.size() / 2));
            std::vector<hs2::MidiEvent> seg;
            if (pos == start)
                for (size_t k = ei; k < ej; ++k) {
                    hs2::MidiEvent e = events[k];
                    e.offset = std::clamp(e.offset - start, 0, n - 1);
                    seg.push_back(e);
                }
            std::fill(buf.begin(), buf.begin() + 2 * n, 0.f);
            engine->processBlock(buf.data(), n, seg.empty() ? nullptr : seg.data(), int(seg.size()),
                                 pos == 0 ? &tr : nullptr);
            for (int i = 0; i < 2 * n; ++i)
                out[2 * pos + i] = int16_t(std::lrint(std::clamp(buf[size_t(i)], -1.f, 1.f) * 32767.f));
            pos += n;
        }
        for (const auto& r : engine->takeProgramRequests()) request(r.channel, r.bank, r.program);
        ei = ej;
        start = endF;
    }

    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double load = dt / (double(frames) / sr);
    cpuAvg += (load - cpuAvg) * 0.02;
    if (load > cpuPeak || --cpuPeakHold <= 0) {
        cpuPeak = load;
        cpuPeakHold = int(sr / frames);  // about a second
    }
}

// ------------------------------------------------------------------------------------------------ JSON for the UI

std::string partsJson(Instance& I) {
    std::string s = "[";
    for (int p = 0; p < kParts; ++p) {
        const auto& pi = I.parts[size_t(p)];
        if (p) s += ",";
        s += "{\"name\":" + jstr(pi.name) + ",\"bank\":" + jstr(pi.bankName) + ",\"bn\":" + std::to_string(pi.bank) +
             ",\"num\":" + std::to_string(pi.prog + 1) + ",\"mute\":" + std::to_string(pi.mute) +
             ",\"solo\":" + std::to_string(pi.solo) + ",\"lock\":" + std::to_string(pi.lock) +
             ",\"used\":" + std::to_string(pi.used) + ",\"voices\":" +
             std::to_string(I.engine ? I.engine->partVoices(p) : 0) + ",\"notes\":" +
             std::to_string(I.noteCount[size_t(p)]) + ",\"missing\":" + jstr(pi.missing) + ",\"level\":" +
             fnum(I.engine ? I.engine->getParameter(p * kPP + 2) : 0) + ",\"pan\":" +
             fnum(I.engine ? I.engine->getParameter(p * kPP + 3) : 0.5) + "}";
    }
    return s + "]";
}

std::string posJson(Instance& I) {
    std::string s = "{\"play\":" + std::to_string(I.playing || I.startWhenPrepared) +
                    ",\"prep\":" + std::to_string(I.preparing) + ",\"preptotal\":" + std::to_string(I.prepTotal) +
                    ",\"sync\":" + std::to_string(I.set.tempo) + ",\"wait\":" + std::to_string(I.syncWaiting) +
                    ",\"end\":" + std::to_string(I.atEnd) + ",\"loop\":" + std::to_string(I.set.loop);
    if (const smf::Song* sg = I.song.song.get()) {
        int bar;
        double beat;
        sg->barBeat(I.posTick, bar, beat);
        const smf::Sig& g = sg->sigAt(uint32_t(I.posTick));
        double bpm = sg->bpmAt(uint32_t(I.posTick));
        if (I.set.tempo == 1 && g_host && g_host->get_bpm) bpm = double(g_host->get_bpm());
        s += ",\"sec\":" + fnum(I.posSec, 2) + ",\"len\":" + fnum(sg->endSec, 2) + ",\"bar\":" + std::to_string(bar + 1) +
             ",\"bars\":" + std::to_string(sg->bars.size()) + ",\"beat\":" + std::to_string(int(beat) + 1) +
             ",\"num\":" + std::to_string(g.num) + ",\"den\":" + std::to_string(g.den) + ",\"bpm\":" + fnum(bpm, 1);
    }
    return s + "}";
}

std::string songJson(Instance& I) {
    if (!I.song.song)
        return "{\"loaded\":0,\"error\":" + jstr(I.songError) + "}";
    const auto& sg = *I.song.song;
    int used = 0;
    for (const auto& p : I.parts) used += p.used;
    return "{\"loaded\":1,\"name\":" + jstr(I.song.name) + ",\"path\":" + jstr(I.song.path) +
           ",\"title\":" + jstr(ascii(sg.title)) + ",\"format\":" + std::to_string(sg.format) +
           ",\"tracks\":" + std::to_string(sg.tracks) + ",\"parts\":" + std::to_string(used) +
           ",\"len\":" + fnum(sg.endSec, 2) + ",\"bars\":" + std::to_string(sg.bars.size()) +
           ",\"bpm\":" + fnum(sg.bpmAt(0), 1) + ",\"patches\":" + std::to_string(I.song.requests.size()) + "}";
}

// ------------------------------------------------------------------------------------------------ plugin API v2

void* v2_create(const char* moduleDir, const char* /*defaults*/) {
    auto* I = new Instance();
    I->moduleDir = moduleDir ? moduleDir : ".";
    I->contentRoot = fs::path(I->moduleDir) / "content";
    if (const char* env = std::getenv("HS2_CONTENT"); env && *env) I->contentRoot = env;  // desktop tests
    if (const char* env = std::getenv("HS2_PRELOAD_MB"); env && *env)
        I->preloadBudget = size_t(std::max(0.0, std::atof(env)) * 1048576.0);
    I->loadSettings();
    I->buf.resize(2 * 4096);
    I->events.reserve(1024);
    I->worker = std::thread([I] { I->workerLoop(); });
    logf("instance created, module folder %s", I->moduleDir.c_str());
    return I;
}

void v2_destroy(void* inst) {
    auto* I = static_cast<Instance*>(inst);
    if (!I) return;
    {
        std::lock_guard<std::mutex> lk(I->qmu);
        I->quit = true;
    }
    I->cv.notify_all();
    if (I->worker.joinable()) I->worker.join();
    {
        std::lock_guard<std::mutex> lk(I->mu);
        I->engine.reset();
    }
    delete I;
}

void v2_on_midi(void* inst, const uint8_t* msg, int len, int source) {
    auto* I = static_cast<Instance*>(inst);
    if (!I || !msg || len < 1) return;
    if (source != MOVE_MIDI_SOURCE_EXTERNAL) return;  // Move's own controls belong to the UI
    const uint8_t st = msg[0];
    if (st < 0x80 || st >= 0xf0) return;
    std::lock_guard<std::mutex> lk(I->mu);
    if (!I->set.usb || !I->engine) return;
    hs2::MidiEvent e;
    e.data[0] = st;
    e.data[1] = len > 1 ? msg[1] & 0x7f : 0;
    e.data[2] = len > 2 ? msg[2] & 0x7f : 0;
    if (I->live.size() < 2048) I->live.push_back(e);
}

int stepBool(int cur, const std::string& v) {
    if (v == "toggle") return !cur;
    return std::atoi(v.c_str()) != 0;
}

void v2_set_param(void* inst, const char* key, const char* val) {
    auto* I = static_cast<Instance*>(inst);
    if (!I || !key) return;
    const std::string k = key, v = val ? val : "";
    if (k == "rescan") {
        {
            std::lock_guard<std::mutex> lk(I->qmu);
            I->scanRequested = true;
        }
        I->cv.notify_one();
        return;
    }
    if (k == "file_path" || k == "open") {
        if (v.empty()) return;
        {
            std::lock_guard<std::mutex> lk(I->mu);
            std::error_code ec;
            const auto dir = fs::path(v).parent_path().string();
            if (!dir.empty() && dir != I->set.lastDir) {
                I->set.lastDir = dir;
                I->saveSettings();
            }
        }
        {
            std::lock_guard<std::mutex> lk(I->qmu);
            I->openRequest = v;
            ++I->fileGen;
        }
        I->cv.notify_one();
        return;
    }
    std::lock_guard<std::mutex> lk(I->mu);
    if (k.rfind("cfg:", 0) == 0) {
        const std::string n = k.substr(4);
        auto& s = I->set;
        if (n == "tempo") {
            s.tempo = stepBool(s.tempo, v);
            if (I->playing) I->stop();
            I->moveRunning = false;
        } else if (n == "gm") {
            s.gm = stepBool(s.gm, v);
            if (I->engine) I->engine->setGmMode(s.gm != 0);
            if (!I->song.path.empty()) {  // the file's program changes now mean other patches: read it again
                std::lock_guard<std::mutex> q(I->qmu);
                I->openRequest = I->song.path;
                ++I->fileGen;
                I->cv.notify_one();
            }
        } else if (n == "usb") {
            s.usb = stepBool(s.usb, v);
            if (!s.usb && I->engine) I->notesOff();
        } else if (n == "loop") {
            s.loop = stepBool(s.loop, v);
        } else if (n == "preload") {
            s.preload = stepBool(s.preload, v);
        } else if (n == "ringout") {
            s.ringOut = stepBool(s.ringOut, v);
            if (I->engine) I->engine->setRingOut(s.ringOut != 0);
        } else if (n == "master") {
            const float cur = I->engine ? I->engine->getParameter(0) : std::max(0.f, s.master);
            s.master = std::clamp(v.size() > 1 && (v[0] == '+' || v[0] == '-') ? cur + float(std::atof(v.c_str())) * 0.01f
                                                                                   : float(std::atof(v.c_str())),
                                  0.f, 1.f);
            if (I->engine) I->engine->setParameter(0, s.master);
        } else {
            return;
        }
        I->saveSettings();
        return;
    }
    if (k == "lastdir") {
        I->set.lastDir = v;
        I->saveSettings();
        return;
    }
    if (!I->engine) return;
    if (k == "transport") {
        if (v == "play") I->play();
        else if (v == "stop") I->stop();
        else if (v == "toggle") (I->playing || I->startWhenPrepared) ? I->stop() : I->play();
        else if (v == "rewind") I->seekTick(0);
        return;
    }
    if (k == "seek_bar" || k == "step_bar") {
        if (!I->song.song) return;
        const auto& s = *I->song.song;
        int bar;
        double beat;
        s.barBeat(I->posTick, bar, beat);
        int target = k == "seek_bar" ? std::atoi(v.c_str()) - 1 : bar + std::atoi(v.c_str());
        // stepping back from inside a bar goes to its start first
        if (k == "step_bar" && std::atoi(v.c_str()) < 0 && beat > 0.05) ++target;
        target = std::clamp(target, 0, int(s.bars.size()) - 1);
        I->seekTick(double(s.barTick(target)));
        return;
    }
    if (k == "panic") {
        I->notesOff();
        return;
    }
    const auto colon = k.find(':');
    if (colon == std::string::npos) return;
    const std::string name = k.substr(0, colon);
    const int p = std::atoi(k.c_str() + colon + 1);
    if (p < 0 || p >= kParts) return;
    PartInfo& pi = I->parts[size_t(p)];
    if (name == "mute") {
        pi.mute = stepBool(pi.mute, v);
        I->updateMutes();
    } else if (name == "solo") {
        pi.solo = stepBool(pi.solo, v);
        I->updateMutes();
    } else if (name == "lock") {
        pi.lock = stepBool(pi.lock, v);
    } else if (name == "level" || name == "pan") {
        const int j = name == "level" ? 2 : 3;
        const float cur = I->engine->getParameter(p * kPP + j);
        const float nv = v.size() > 1 && (v[0] == '+' || v[0] == '-') ? cur + float(std::atof(v.c_str())) * 0.01f
                                                                       : float(std::atof(v.c_str()));
        I->engine->setParameter(p * kPP + j, std::clamp(nv, 0.f, 1.f));
    } else if (name == "load") {  // "<bank number>.<patch index>": chosen by hand, the part is locked to it
        const auto dot = v.find('.');
        if (dot == std::string::npos) return;
        const int bank = std::atoi(v.c_str()), idx = std::atoi(v.c_str() + dot + 1);
        if (bank < 0 || bank > 255 || idx < 0 || idx > 255) return;
        pi.lock = true;
        I->applyKey(p, keyOf(bank, idx), true);
    }
}

int v2_get_param(void* inst, const char* key, char* buf, int len) {
    auto* I = static_cast<Instance*>(inst);
    if (!I || !key) return -1;
    const std::string k = key;
    if (k == "status") return writeOut(buf, len, I->getStatus());
    if (k == "module_id") return writeOut(buf, len, "hs2-midi-player");
    std::lock_guard<std::mutex> lk(I->mu);
    if (k == "ready") return writeOut(buf, len, std::to_string(I->scanState));
    if (k == "song") return writeOut(buf, len, songJson(*I));
    if (k == "pos") return writeOut(buf, len, posJson(*I));
    if (k == "parts") return writeOut(buf, len, partsJson(*I));
    if (k == "cpu")
        return writeOut(buf, len, std::to_string(int(std::lround(I->cpuAvg * 100))) + "," +
                                      std::to_string(int(std::lround(I->cpuPeak * 100))) + "," +
                                      std::to_string(I->engine ? I->engine->totalVoices() : 0));
    if (k == "mem") return writeOut(buf, len, std::to_string(I->content ? I->content->decodedSampleBytes() : 0));
    if (k == "cfg") {
        const auto& s = I->set;
        const float master = I->engine ? I->engine->getParameter(0) : std::max(0.f, s.master);
        return writeOut(buf, len, "{\"tempo\":" + std::to_string(s.tempo) + ",\"gm\":" + std::to_string(s.gm) +
                                      ",\"usb\":" + std::to_string(s.usb) + ",\"loop\":" + std::to_string(s.loop) +
                                      ",\"preload\":" + std::to_string(s.preload) + ",\"ringout\":" +
                                      std::to_string(s.ringOut) + ",\"master\":" + fnum(master) + ",\"lastdir\":" +
                                      jstr(s.lastDir) + "}");
    }
    if (k == "banks") {
        std::string s = "[";
        for (size_t i = 0; i < I->banks.size(); ++i)
            s += (i ? "," : "") + std::string("[") + std::to_string(I->banks[i].number) + "," + jstr(I->banks[i].name) +
                 "," + std::to_string(I->banks[i].patches.size()) + "]";
        return writeOut(buf, len, s + "]");
    }
    if (k.rfind("bankp:", 0) == 0) {  // a bank's patches by row: [[index, name], ...]
        const size_t r = size_t(std::atoi(k.c_str() + 6));
        if (r >= I->banks.size()) return writeOut(buf, len, "[]");
        std::string s = "[";
        const auto& ps = I->banks[r].patches;
        for (size_t i = 0; i < ps.size(); ++i)
            s += (i ? "," : "") + std::string("[") + std::to_string(ps[i].first) + "," + jstr(ps[i].second) + "]";
        return writeOut(buf, len, s + "]");
    }
    return -1;
}

int v2_get_error(void* inst, char* buf, int len) {
    auto* I = static_cast<Instance*>(inst);
    if (!I) return 0;
    std::lock_guard<std::mutex> lk(I->mu);
    if (I->scanState != -1) return 0;
    return writeOut(buf, len, "Hypersonic 2 content not found");
}

void v2_render(void* inst, int16_t* out, int frames) {
    auto* I = static_cast<Instance*>(inst);
    if (!out || frames <= 0) return;
    std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
    if (!I) return;
    std::lock_guard<std::mutex> lk(I->mu);
    if (!I->engine) {
        I->live.clear();
        return;
    }
    I->render(out, frames);
}

plugin_api_v2_t g_api = {
    MOVE_PLUGIN_API_VERSION_2, v2_create, v2_destroy, v2_on_midi, v2_set_param, v2_get_param, v2_get_error, v2_render,
};

}  // namespace

extern "C" __attribute__((visibility("default"))) plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host) {
    g_host = host;
    return &g_api;
}
