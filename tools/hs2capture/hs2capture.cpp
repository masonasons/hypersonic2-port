// hs2capture: hosts the original Hypersonic 2 VST 2 plugin (32-bit Windows DLL, licensed and activated) and, for each
// patch in a list, loads it on one part through MIDI bank select + program change, then
//   - patch:     saves the patch itself as a plain "StSS" .stss file, and
//   - snapshot:  saves the plugin's state as .fxb (bank chunk = the whole plugin) and .fxp (program chunk), and/or
//   - record:    plays notes and writes 32-bit float WAV files plus an SFZ file mapping them.
// Patch mode, for the copy-protected patches: they are stored encrypted, and the plugin unprotects each one as
// it loads it, keeping it only in memory (its state, asked for through the VST 2 chunk interface, is always
// re-encrypted on the way out). The host reads that plain patch from the plugin's memory as the plugin frees
// the decrypted buffer after the load: one imported function pointer (the plugin's own operator delete) is
// redirected, and only buffers that are the plain "StSS" blob of the patch asked for are kept. No code is
// modified, nothing is decrypted here, and the plugin does the loading under its licence; the port loads the
// .stss files exactly like the stored non-protected patches. Captures are personal use under your licence,
// like the rest of your content: keep them on your machine, do not distribute them.
//
// Usage: hs2capture --list patches.tsv --out DIR [options]   (hs2capture --help lists them)
// patches.tsv: one patch per line, "bank<TAB>index<TAB>name"; '#' starts a comment
// (tools/formats/list_protected.py --tsv writes one).
//
// On Windows the plugin's editor is opened in a window and the message loop is pumped while patches load: the
// original runs its patch loader from its GUI / idle code. Elsewhere (the self-test against tests/mock_plugin.cpp)
// the plugin is loaded with dlopen and only effIdle / effEditIdle are called.
#include "vst2abi.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- options

struct Options {
    std::string plugin, list, out = "hs2capture-out";
    bool snapshot = true, record = true, patch = false, gui = true, overwrite = false;
    int part = 1;                  // MIDI channel / part 1..16
    double sampleRate = 44100;
    int block = 512;
    double settle = 20;            // longest wait for a patch to load, seconds
    double speed = 1;              // 1 = real time, 0 = as fast as possible
    std::vector<int> notes, vels{127, 64};
    double hold = 2.0, tail = 6.0; // seconds held / longest release recorded
    double quietDb = -90;
    int limit = 0;
    int maxFails = 3;             // stop the run after this many consecutive failed loads (a wedged plugin
                                   // fails every patch after that; the run is restarted fresh instead)
};

std::vector<int> parseNotes(const std::string& s) {
    // "36-96/3" (from-to/step) or "36,48,60"
    std::vector<int> r;
    int a, b, st;
    if (std::sscanf(s.c_str(), "%d-%d/%d", &a, &b, &st) == 3 && st > 0) {
        for (int n = a; n <= b; n += st) r.push_back(n);
    } else if (std::sscanf(s.c_str(), "%d-%d", &a, &b) == 2) {
        for (int n = a; n <= b; ++n) r.push_back(n);
    } else {
        std::stringstream ss(s);
        std::string t;
        while (std::getline(ss, t, ',')) r.push_back(std::atoi(t.c_str()));
    }
    r.erase(std::remove_if(r.begin(), r.end(), [](int n) { return n < 0 || n > 127; }), r.end());
    return r;
}

void usage() {
    std::puts(
        "hs2capture --list patches.tsv [--out DIR] [options]\n"
        "  --plugin PATH     Hypersonic 2 DLL (default: looked for in the usual VST folders)\n"
        "  --mode M          patch, snapshot, record, both (snapshot+record) or all (default both)\n"
        "  --part N          part / MIDI channel the patches load on, 1-16 (default 1)\n"
        "  --notes SPEC      notes recorded: from-to/step or a list (default 24-96/3)\n"
        "  --vels LIST       velocities recorded (default 127,64)\n"
        "  --hold SEC        how long each note is held (default 2)\n"
        "  --tail SEC        longest release recorded after the note off (default 6)\n"
        "  --quiet-db DB     level counted as silence (default -90)\n"
        "  --settle SEC      longest wait for a patch to load (default 20)\n"
        "  --speed X         1 = real time (default), 2 = twice as fast, 0 = as fast as possible\n"
        "  --rate HZ         sample rate (default 44100)\n"
        "  --block N         block size (default 512)\n"
        "  --limit N         only the first N patches of the list\n"
        "  --max-fails N     stop the run after N consecutive failed loads (default 3; the original's loader\n"
        "                    can stop loading copy-protected patches after a while - a fresh process continues)\n"
        "  --no-gui          do not open the plugin's editor window\n"
        "  --overwrite       redo patches whose files exist (default: skipped, so a run can be resumed)");
}

// ---------------------------------------------------------------- plugin host

struct Host;
Host* gHost = nullptr;

struct Host {
    Options opt;
    AEffect* fx = nullptr;
    VstTimeInfo time{};
    std::string pluginDir;
    double samplePos = 0;
    std::vector<std::vector<float>> outBuf, inBuf;
    std::vector<float*> outPtr, inPtr;
    std::chrono::steady_clock::time_point clock0;
    double pacedFrames = 0;
#if defined(_WIN32)
    HMODULE lib = nullptr;
    HWND wnd = nullptr;
#else
    void* lib = nullptr;
#endif
    // The last moment the plugin made progress (a process or pump call returned). The watchdog exits the
    // process when the plugin stops responding to its own idle calls - a wedged original never returns from
    // its window / loader / licence code, and nothing in-process can recover from that; the outer
    // capture_all.ps1 restarts and continues.
    std::chrono::steady_clock::time_point lastProgress{};

    void progress() { lastProgress = std::chrono::steady_clock::now(); }

    intptr_t call(int32_t op, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt_ = 0) {
        return fx->dispatcher(fx, op, index, value, ptr, opt_);
    }

    static intptr_t VSTCALL callback(AEffect* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt_) {
        (void)e; (void)index; (void)value; (void)opt_;
        Host* h = gHost;
        switch (op) {
            case audioMasterVersion: return 2400;
            case audioMasterCurrentId: return 0;
            case audioMasterIdle: return 0;
            case audioMasterWantMidi: return 1;
            case audioMasterGetTime: return h ? intptr_t(&h->time) : 0;
            case audioMasterIOChanged: return 1;
            case audioMasterNeedIdle: return 1;
            case audioMasterSizeWindow:
#if defined(_WIN32)
                if (h && h->wnd) {
                    RECT r{0, 0, LONG(index), LONG(value)};
                    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                    SetWindowPos(h->wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
#endif
                return 1;
            case audioMasterGetSampleRate: return h ? intptr_t(h->opt.sampleRate) : 44100;
            case audioMasterGetBlockSize: return h ? h->opt.block : 512;
            case audioMasterGetCurrentProcessLevel: return 0;
            case audioMasterGetAutomationState: return 1;
            case audioMasterGetVendorString: std::strcpy(static_cast<char*>(ptr), "hs2capture"); return 1;
            case audioMasterGetProductString: std::strcpy(static_cast<char*>(ptr), "hs2capture"); return 1;
            case audioMasterGetVendorVersion: return 1;
            case audioMasterCanDo: {
                const char* s = static_cast<const char*>(ptr);
                for (const char* c : {"sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents",
                                      "receiveVstMidiEvent", "sizeWindow", "supplyIdle"})
                    if (s && std::strcmp(s, c) == 0) return 1;
                return 0;
            }
            case audioMasterGetLanguage: return 1;  // English
            case audioMasterGetDirectory: return h ? intptr_t(h->pluginDir.c_str()) : 0;
            default: return 0;
        }
    }

    bool load(const std::string& path) {
        PluginEntryProc entry = nullptr;
        pluginDir = fs::path(path).parent_path().string();
#if defined(_WIN32)
        SetDllDirectoryA(pluginDir.c_str());
        lib = LoadLibraryA(path.c_str());
        if (!lib) {
            std::fprintf(stderr, "cannot load %s (Windows error %lu)\n", path.c_str(), GetLastError());
            return false;
        }
        entry = reinterpret_cast<PluginEntryProc>(reinterpret_cast<void*>(GetProcAddress(lib, "VSTPluginMain")));
        if (!entry) entry = reinterpret_cast<PluginEntryProc>(reinterpret_cast<void*>(GetProcAddress(lib, "main")));
#else
        lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            std::fprintf(stderr, "cannot load %s: %s\n", path.c_str(), dlerror());
            return false;
        }
        entry = reinterpret_cast<PluginEntryProc>(dlsym(lib, "VSTPluginMain"));
#endif
        if (!entry) {
            std::fprintf(stderr, "%s is not a VST 2 plugin (no entry point)\n", path.c_str());
            return false;
        }
        fx = entry(&Host::callback);
        if (!fx || fx->magic != kVstMagic) {
            std::fprintf(stderr, "the plugin did not start (no AEffect). Is it licensed on this computer?\n");
            fx = nullptr;
            return false;
        }
        if (!(fx->flags & effFlagsCanReplacing) || !fx->processReplacing) {
            std::fprintf(stderr, "the plugin has no processReplacing\n");
            return false;
        }
        call(effOpen);
        call(effSetSampleRate, 0, 0, nullptr, float(opt.sampleRate));
        call(effSetBlockSize, 0, opt.block);
        outBuf.assign(size_t(std::max(2, fx->numOutputs)), std::vector<float>(size_t(opt.block)));
        inBuf.assign(size_t(std::max(0, fx->numInputs)), std::vector<float>(size_t(opt.block)));
        for (auto& b : outBuf) outPtr.push_back(b.data());
        for (auto& b : inBuf) inPtr.push_back(b.data());
        call(effMainsChanged, 0, 1);
        call(effStartProcess);
        char name[256] = {}, vendor[256] = {};
        call(effGetEffectName, 0, 0, name);
        call(effGetVendorString, 0, 0, vendor);
        std::printf("plugin: %s (%s), id %08x, version %d, %d outputs, %d programs, chunks %s\n", name, vendor,
                    unsigned(fx->uniqueID), fx->version, fx->numOutputs, fx->numPrograms,
                    (fx->flags & effFlagsProgramChunks) ? "yes" : "no");
        if (opt.gui) openEditor();
        clock0 = std::chrono::steady_clock::now();
        return true;
    }

    void openEditor() {
#if defined(_WIN32)
        if (!(fx->flags & effFlagsHasEditor)) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "hs2capture";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        ERect* er = nullptr;
        call(effEditGetRect, 0, 0, &er);
        int w = 800, hgt = 600;
        if (er) { w = er->right - er->left; hgt = er->bottom - er->top; }
        RECT r{0, 0, w, hgt};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        wnd = CreateWindowA("hs2capture", "hs2capture - Hypersonic 2", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance,
                            nullptr);
        if (!wnd) return;
        ShowWindow(wnd, SW_SHOWNOACTIVATE);
        call(effEditOpen, 0, 0, wnd);
        call(effEditGetRect, 0, 0, &er);  // some plugins know their size only once open
        if (er) callback(fx, audioMasterSizeWindow, er->right - er->left, er->bottom - er->top, nullptr, 0);
#endif
    }

    // Window messages, editor idle and plugin idle: where the original loads patches.
    void pump() {
#if defined(_WIN32)
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (wnd) call(effEditIdle);
#endif
        call(effIdle);
        progress();
    }

    void close() {
        if (!fx) return;
#if defined(_WIN32)
        if (wnd) {
            call(effEditClose);
            DestroyWindow(wnd);
            wnd = nullptr;
        }
#endif
        call(effStopProcess);
        call(effMainsChanged, 0, 0);
        call(effClose);
        fx = nullptr;
    }

    // One block: the events (deltaFrames inside the block), then processReplacing. Out: stereo, outputs 1 and 2.
    void process(const std::vector<VstMidiEvent>& ev, float* left, float* right) {
        progress();
        const double tempo = 120;
        time.samplePos = samplePos;
        time.sampleRate = opt.sampleRate;
        time.nanoSeconds = samplePos / opt.sampleRate * 1e9;
        time.ppqPos = samplePos / opt.sampleRate * tempo / 60.0;
        time.tempo = tempo;
        time.barStartPos = std::floor(time.ppqPos / 4) * 4;
        time.timeSigNumerator = 4;
        time.timeSigDenominator = 4;
        time.flags = kVstTransportPlaying | kVstNanosValid | kVstPpqPosValid | kVstTempoValid | kVstBarsValid |
                     kVstTimeSigValid;
        if (!ev.empty()) {
            static VstEventsN evs;
            static std::vector<VstMidiEvent> store;
            store = ev;
            evs.numEvents = int32_t(std::min<size_t>(store.size(), kMaxEvents));
            evs.reserved = 0;
            for (int i = 0; i < evs.numEvents; ++i) evs.events[i] = &store[size_t(i)];
            call(effProcessEvents, 0, 0, &evs);
        }
        for (auto& b : inBuf) std::fill(b.begin(), b.end(), 0.f);
        for (auto& b : outBuf) std::fill(b.begin(), b.end(), 0.f);
        fx->processReplacing(fx, inPtr.empty() ? nullptr : inPtr.data(), outPtr.data(), opt.block);
        if (left) std::copy(outBuf[0].begin(), outBuf[0].end(), left);
        if (right) std::copy(outBuf[1].begin(), outBuf[1].end(), right);
        samplePos += opt.block;
        pace();
    }

    // Keep the rendering at opt.speed times real time (the plugin's streaming and loader expect real time).
    void pace() {
        pacedFrames += opt.block;
        if (opt.speed <= 0) return;
        const double due = pacedFrames / opt.sampleRate / opt.speed;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count();
        if (due > now + 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(due - now));
        else if (now - due > 1.0) {  // far behind (a slow load): do not catch up in a burst
            pacedFrames = now * opt.sampleRate * opt.speed;
        }
    }

    std::vector<uint8_t> chunk(bool program) {
        void* p = nullptr;
        const intptr_t n = call(effGetChunk, program ? 1 : 0, 0, &p);
        if (n <= 0 || !p) return {};
        const auto* b = static_cast<const uint8_t*>(p);
        return std::vector<uint8_t>(b, b + n);
    }

    bool setChunk(bool program, const uint8_t* data, size_t n) {
        return call(effSetChunk, program ? 1 : 0, intptr_t(n), const_cast<uint8_t*>(data)) != 0;
    }
};

VstMidiEvent midi(int delta, uint8_t a, uint8_t b, uint8_t c) {
    VstMidiEvent e{};
    e.type = kVstMidiType;
    e.byteSize = sizeof(VstMidiEvent);
    e.deltaFrames = delta;
    e.midiData[0] = char(a);
    e.midiData[1] = char(b);
    e.midiData[2] = char(c);
    return e;
}

// ---------------------------------------------------------------- files

void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

// VST 2 preset files: 'CcnK' header, big-endian; FBCh (bank chunk) or FPCh (program chunk).
bool writeFx(const fs::path& path, const AEffect* fx, const std::vector<uint8_t>& data, bool bank,
             const std::string& name) {
    std::vector<uint8_t> v;
    v.insert(v.end(), {'C', 'c', 'n', 'K'});
    be32(v, 0);  // size, filled below
    if (bank) v.insert(v.end(), {'F', 'B', 'C', 'h'});
    else v.insert(v.end(), {'F', 'P', 'C', 'h'});
    be32(v, 1);  // format version
    be32(v, uint32_t(fx->uniqueID));
    be32(v, uint32_t(fx->version));
    be32(v, uint32_t(bank ? std::max(1, fx->numPrograms) : 1));
    if (bank) {
        v.insert(v.end(), 128, 0);  // future
    } else {
        char prg[28] = {};
        std::strncpy(prg, name.c_str(), sizeof(prg) - 1);
        v.insert(v.end(), prg, prg + 28);
    }
    be32(v, uint32_t(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    const uint32_t size = uint32_t(v.size() - 8);
    for (int i = 0; i < 4; ++i) v[size_t(4 + i)] = uint8_t(size >> (24 - 8 * i));
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size()));
    return bool(f);
}

void le(std::ofstream& f, uint32_t x, int n) {
    for (int i = 0; i < n; ++i) f.put(char(x >> (8 * i)));
}

bool writeWav(const fs::path& path, const std::vector<float>& lr, double sr, int rootNote) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t data = uint32_t(lr.size() * 4);
    const uint32_t smpl = 36;  // 9 fields, no loops
    f.write("RIFF", 4);
    le(f, 4 + (8 + 16) + (8 + 4) + (8 + smpl) + (8 + data), 4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    le(f, 16, 4);
    le(f, 3, 2);  // IEEE float
    le(f, 2, 2);
    le(f, uint32_t(sr), 4);
    le(f, uint32_t(sr) * 8, 4);
    le(f, 8, 2);
    le(f, 32, 2);
    f.write("fact", 4);
    le(f, 4, 4);
    le(f, uint32_t(lr.size() / 2), 4);
    f.write("smpl", 4);  // the root note, for samplers that read it; no loops
    le(f, smpl, 4);
    le(f, 0, 4);
    le(f, 0, 4);
    le(f, uint32_t(1e9 / sr), 4);
    le(f, uint32_t(rootNote), 4);
    le(f, 0, 4);  // pitch fraction
    le(f, 0, 4);  // SMPTE format
    le(f, 0, 4);  // SMPTE offset
    le(f, 0, 4);  // loops
    le(f, 0, 4);  // sampler data
    f.write("data", 4);
    le(f, data, 4);
    f.write(reinterpret_cast<const char*>(lr.data()), std::streamsize(data));
    return bool(f);
}

const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
std::string noteName(int n) { return std::string(kNoteNames[n % 12]) + std::to_string(n / 12 - 1); }

std::string safeName(const std::string& s) {
    std::string r;
    for (unsigned char c : s) r += (std::isalnum(c) || std::strchr(" -_+().,&'!", c)) ? char(c) : '_';
    while (!r.empty() && (r.back() == ' ' || r.back() == '.')) r.pop_back();
    return r.empty() ? "patch" : r;
}

struct Patch {
    int bank = 0, index = 0;
    std::string name;
};

std::vector<Patch> readList(const std::string& path) {
    std::vector<Patch> r;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string b, i, n;
        if (!std::getline(ss, b, '\t') || !std::getline(ss, i, '\t')) continue;
        std::getline(ss, n);
        Patch p{std::atoi(b.c_str()), std::atoi(i.c_str()), n};
        if (p.bank < 0 || p.bank > 16383 || p.index < 0 || p.index > 127) {
            std::fprintf(stderr, "skipped line (bank 0-16383, index 0-127): %s\n", line.c_str());
            continue;
        }
        r.push_back(p);
    }
    return r;
}

struct PatchHook;  // Windows-only below; other platforms never enable --mode patch
#if defined(_WIN32)
// ---------------------------------------------------------------- patch capture
//
// The copy-protected patches are stored encrypted; the licensed plugin unprotects each one as it loads it (its
// program-change path, loadPatchFromLibrary in the ghidra decomp, frees the decrypted patch buffer with
// operator delete right after the load). So this hooks the plugin's imported operator delete - one pointer in
// its import table, no code modification - and keeps the freed buffer when it is the patch currently loading:
// a plain "StSS" blob whose own name field (+0x20) matches. The blob is self-describing, so its structural end
// (0x620 header + element and send-FX records + the trailer: marker, CC map, phrase slots) gives its length,
// as the port's Patch::parse and Arp::patchTrailer read it. The port then loads the .stss files exactly like
// the stored non-protected patches; it never decrypts anything itself. Nothing here touches the protection:
// the plugin does the loading, under its licence, and the capture is what it loaded.
struct PatchHook;
PatchHook* gHook = nullptr;

struct PatchHook {
    static constexpr size_t kMaxBlob = 256 * 1024;

    using DeleteProc = void(__cdecl*)(void*);
    void** slot = nullptr;      // the plugin's IAT entry for operator delete
    DeleteProc realDelete = nullptr;
    bool installed = false;
    std::mutex mu;
    std::vector<std::vector<uint8_t>> caps;  // plain StSS blobs of the wanted patch
    std::string want;                         // normalized name of the patch being loaded (the filter)

    static uint32_t rd32(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }

    // The structural end of a plain patch blob, as the port's Patch::parse and Arp::patchTrailer read it.
    // Returns 0 when the data at `d` is not a patch (the plugin frees plenty of other buffers).
    static size_t patchEnd(const uint8_t* d) {
        const auto at = [d](size_t o, uint32_t& v) {
            if (o + 4 > kMaxBlob) return false;
            v = rd32(d + o);
            return true;
        };
        if (std::memcmp(d, "StSS", 4) != 0) return 0;
        uint32_t version = 0;
        if (!at(4, version) || version > 219999999) return 0;  // the loader's own limit
        uint32_t elements = 0;
        if (!at(0x14, elements) || elements > 64) return 0;
        int nfx = 0;
        for (int i = 0; i < 4; ++i)
            if (d[0x18 + size_t(i)]) ++nfx;
        size_t end = 0x620 + (size_t(elements) + size_t(nfx)) * 0x420;
        uint32_t marker = 0;
        if (!at(end, marker)) return 0;
        end += 4;
        if (marker == 2) {  // CC map + embedded phrase slots
            end += 0x100;
            uint32_t last = 0;
            if (!at(end, last) || last > 63) return 0;
            end += 4;
            for (uint32_t i = 0; i <= last; ++i) {
                uint32_t sz = 0;
                if (!at(end, sz)) return 0;
                if (sz == 0) {
                    end += 4;
                    continue;
                }
                if (sz > 64 * 1024 || end + sz > kMaxBlob) return 0;
                end += sz;
            }
        }
        return end <= kMaxBlob ? end : 0;
    }

    // Finds the imported scalar "operator delete" ("??3@...") in the module at `base` and redirects it.
    bool install(uintptr_t base) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(base);
        if (std::memcmp(m, "MZ", 2) != 0) return false;
        const uint8_t* pe = m + rd32(m + 0x3c);
        if (std::memcmp(pe, "PE\0\0", 4) != 0) return false;
        const uint32_t rva = rd32(pe + 0x78 + 8);  // data directory 1: the import table
        if (rva == 0) return false;
        for (const uint8_t* imp = m + rva; rd32(imp + 12) != 0; imp += 20) {
            const uint32_t orig = rd32(imp), first = rd32(imp + 16);
            if (orig == 0 || first == 0) continue;
            for (const uint8_t *t = m + orig, *iat = m + first;; t += 4, iat += 4) {
                const uint32_t nameRva = rd32(t);
                if (nameRva == 0) break;
                if (nameRva & 0x80000000u) continue;  // by ordinal
                if (std::memcmp(m + nameRva + 2, "??3@", 4) != 0) continue;  // operator delete
                slot = reinterpret_cast<void**>(const_cast<uint8_t*>(iat));
                DWORD protect = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
                realDelete = *reinterpret_cast<DeleteProc*>(slot);
                *slot = reinterpret_cast<void*>(&hookedDelete);
                installed = true;
                return true;
            }
        }
        return false;
    }

    void uninstall() {
        if (!installed) return;
        *slot = reinterpret_cast<void*>(realDelete);
        installed = false;
    }

    void wantName(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu);
        want = normName(name);
    }

    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return caps.size();
    }

    // The captures added since index `mark`.
    std::vector<std::vector<uint8_t>> since(size_t mark) {
        std::lock_guard<std::mutex> lk(mu);
        mark = std::min(mark, caps.size());
        return std::vector<std::vector<uint8_t>>(caps.begin() + ptrdiff_t(mark), caps.end());
    }

    // Normalized name: the browser markers and spacing removed, lowercase (the blob's name field and the bank
    // directory can differ in detail).
    static std::string normName(std::string s) {
        s.erase(std::remove_if(s.begin(), s.end(),
                               [](unsigned char c) {
                                   return std::isspace(c) != 0 || std::strchr("[]{}_ ", c) != nullptr;
                               }),
                s.end());
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    // The blob's own name field (+0x20), normalized.
    static std::string blobName(const uint8_t* d) {
        size_t len = 0;
        while (len < 0x20 && d[0x20 + len]) ++len;
        return normName(std::string(reinterpret_cast<const char*>(d) + 0x20, len));
    }

    void take(const uint8_t* p) {
        if (patchEnd(p) == 0) return;  // cheap rejection of every other freed buffer
        std::lock_guard<std::mutex> lk(mu);
        if (want.empty() || blobName(p) != want) return;  // only the patch being loaded
        caps.emplace_back(p, p + patchEnd(p));
    }

private:
    static void __cdecl hookedDelete(void* p) {
        PatchHook* h = gHook;
        if (h != nullptr && p != nullptr) h->take(reinterpret_cast<const uint8_t*>(p));
        h->realDelete(p);
    }
};
#endif

// ---------------------------------------------------------------- capture

struct Capture {
    Host& h;
    const Options& o;
    std::vector<float> l, r;
    PatchHook* hook = nullptr;  // --mode patch (Windows): the loader hook
    size_t hookMark = 0;       // set before loadPatch: when the hook captures this patch, the load is done
    Capture(Host& host) : h(host), o(host.opt), l(size_t(host.opt.block)), r(size_t(host.opt.block)) {}

    int ch() const { return o.part - 1; }

    double peak() const {
        double p = 0;
        for (size_t i = 0; i < l.size(); ++i) p = std::max({p, double(std::fabs(l[i])), double(std::fabs(r[i]))});
        return p;
    }

    // Loads the patch; true once the plugin's state has changed and stayed the same for half a second. In patch
    // mode the hook is the better signal: when the plugin frees the decrypted buffer of the wanted patch, the
    // load has happened, and the state wait can stop at once.
    bool loadPatch(const Patch& p, std::string& why) {
        const std::vector<uint8_t> before = h.chunk(false);
        std::vector<VstMidiEvent> ev{midi(0, uint8_t(0xb0 | ch()), 0x7b, 0),  // all notes off
                                     midi(0, uint8_t(0xb0 | ch()), 0x00, uint8_t(p.bank >> 7)),
                                     midi(0, uint8_t(0xb0 | ch()), 0x20, uint8_t(p.bank & 0x7f)),
                                     midi(0, uint8_t(0xc0 | ch()), uint8_t(p.index), 0)};
        const int checkEvery = std::max(1, int(0.1 * o.sampleRate / o.block));
        const int stableNeeded = 5;  // checks, 0.5 s
        std::vector<uint8_t> last;
        int stable = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0;; ++b) {
            h.process(ev, nullptr, nullptr);
            ev.clear();
            h.pump();
            if (hook != nullptr && !o.snapshot && !o.record && hook->count() > hookMark)
                return true;  // the capture is the load; the state wait is only for snapshots / records
            if (b % checkEvery) continue;
            std::vector<uint8_t> now = h.chunk(false);
            if (now != before && !now.empty()) {
                stable = now == last ? stable + 1 : 0;
                last = std::move(now);
                if (stable >= stableNeeded) return true;
            }
            const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (waited > o.settle && (o.speed > 0 || b * o.block > o.settle * o.sampleRate)) {
                why = last.empty() ? "the plugin's state did not change (patch not loaded: GM mode on, a wrong "
                                     "bank / index, or the load refused)"
                                   : "the plugin's state kept changing";
                return !last.empty();  // still changing: take it, with a warning
            }
        }
    }

    // Renders silence until the output is quiet (or maxSec passed).
    void settleQuiet(double maxSec) {
        const double thr = std::pow(10.0, o.quietDb / 20);
        int quiet = 0;
        const int need = std::max(1, int(0.25 * o.sampleRate / o.block));
        const int maxBlocks = int(maxSec * o.sampleRate / o.block);
        for (int b = 0; b < maxBlocks && quiet < need; ++b) {
            h.process({}, l.data(), r.data());
            h.pump();
            quiet = peak() < thr ? quiet + 1 : 0;
        }
        if (quiet < need) {  // still sounding: cut it
            h.process({midi(0, uint8_t(0xb0 | ch()), 0x78, 0), midi(0, uint8_t(0xb0 | ch()), 0x7b, 0)}, l.data(),
                      r.data());
        }
    }

    // One note: held o.hold seconds, then recorded until quiet or o.tail seconds. Interleaved stereo out.
    std::vector<float> recordNote(int note, int vel) {
        const double thr = std::pow(10.0, o.quietDb / 20);
        const long holdFrames = long(o.hold * o.sampleRate);
        const long maxFrames = holdFrames + long(o.tail * o.sampleRate);
        std::vector<float> out;
        long pos = 0;
        long lastLoud = -1, quietRun = 0;
        bool offSent = false;
        while (pos < maxFrames) {
            std::vector<VstMidiEvent> ev;
            if (pos == 0) ev.push_back(midi(0, uint8_t(0x90 | ch()), uint8_t(note), uint8_t(vel)));
            if (!offSent && holdFrames < pos + o.block) {
                ev.push_back(midi(int(std::max(0L, holdFrames - pos)), uint8_t(0x80 | ch()), uint8_t(note), 64));
                offSent = true;
            }
            h.process(ev, l.data(), r.data());
            h.pump();
            for (int i = 0; i < o.block; ++i) {
                out.push_back(l[size_t(i)]);
                out.push_back(r[size_t(i)]);
                if (std::fabs(l[size_t(i)]) > thr || std::fabs(r[size_t(i)]) > thr) lastLoud = pos + i;
            }
            pos += o.block;
            if (offSent && pos > holdFrames) {
                quietRun = pos - std::max(lastLoud + 1, holdFrames);
                if (quietRun > long(0.3 * o.sampleRate)) break;
            }
        }
        if (!offSent) h.process({midi(0, uint8_t(0x80 | ch()), uint8_t(note), 64)}, nullptr, nullptr);
        // Trim the silence after the last sound (keeping 20 ms), with a 10 ms fade.
        const long keep = std::min<long>(long(out.size() / 2), std::max(lastLoud + 1, 0L) + long(0.02 * o.sampleRate));
        out.resize(size_t(keep) * 2);
        const long fade = std::min<long>(keep, long(0.01 * o.sampleRate));
        for (long i = 0; i < fade; ++i) {
            const float g = float(i) / float(fade);
            out[size_t((keep - 1 - i) * 2)] *= g;
            out[size_t((keep - 1 - i) * 2 + 1)] *= g;
        }
        return out;
    }

    // WAVs + SFZ for one patch. Returns the number of silent notes (skipped).
    int record(const Patch& p, const fs::path& dir, const std::string& base) {
        fs::create_directories(dir);
        settleQuiet(o.tail);
        std::vector<int> vels = o.vels;
        std::sort(vels.begin(), vels.end());
        std::ostringstream sfz;
        sfz << "// " << p.name << " (Hypersonic 2 bank " << p.bank << ", patch " << p.index
            << "), recorded by hs2capture\n<control>\ndefault_path=./\n<global>\nampeg_release=0.3\n";
        int silent = 0;
        int lo = 1;
        for (size_t vi = 0; vi < vels.size(); ++vi) {
            const int vel = vels[vi];
            const int hi = vi + 1 < vels.size() ? (vel + vels[vi + 1]) / 2 : 127;
            sfz << "\n<group>\nlovel=" << lo << " hivel=" << hi << "\n";
            lo = hi + 1;
            std::vector<std::pair<int, std::string>> taken;
            for (int n : o.notes) {
                std::vector<float> s = recordNote(n, vel);
                settleQuiet(o.tail);
                if (s.size() < size_t(0.05 * o.sampleRate) * 2) {
                    ++silent;
                    continue;
                }
                char fn[160];
                std::snprintf(fn, sizeof(fn), "%s n%03d %s v%03d.wav", base.c_str(), n, noteName(n).c_str(), vel);
                writeWav(dir / fn, s, o.sampleRate, n);
                taken.push_back({n, fn});
            }
            for (size_t i = 0; i < taken.size(); ++i) {  // each note covers up to halfway to its neighbours
                const int n = taken[i].first;
                const int lk = i == 0 ? 0 : (taken[i - 1].first + n) / 2 + 1;
                const int hk = i + 1 == taken.size() ? 127 : (n + taken[i + 1].first) / 2;
                sfz << "<region> sample=" << taken[i].second << " pitch_keycenter=" << n << " lokey=" << lk
                    << " hikey=" << hk << "\n";
            }
        }
        std::ofstream(dir / (base + ".sfz")) << sfz.str();
        return silent;
    }

    // --mode patch ------------------------------------------------------------

    // The captured patch, name-checked by the hook (only blobs whose name field matches the patch being
    // loaded are kept).
    std::vector<uint8_t> capturePatch(size_t mark, std::string& detail) {
        const auto list = hook ? hook->since(mark) : std::vector<std::vector<uint8_t>>{};
        if (list.empty()) {
            detail = "the plugin loaded no patch with that name (GM mode on, or the load was refused)";
            return {};
        }
        return list.back();
    }

    // How many patch parameters the original's own loader would warn about (outside 0..1): 0 for a healthy
    // capture, thousands if something went wrong.
    static int outOfRange(const std::vector<uint8_t>& blob) {
        if (blob.size() < 0x620) return 0x100;
        int n = 0;
        for (int i = 0; i < 0x100; ++i) {
            float v = 0;
            std::memcpy(&v, blob.data() + 0x220 + 4 * size_t(i), 4);
            if (!(v >= 0.f && v <= 1.f)) ++n;  // NaN fails both comparisons
        }
        return n;
    }

    // The strongest check the interface allows: feed the captured blob back through setChunk. The plugin's
    // state is always encrypted on the way out (getState runs its Syncrosoft encode over everything), so the
    // bytes cannot be compared - but a plain chunk that the plugin accepts and loads proves the capture is a
    // well-formed patch the original itself will play.
    std::string verify(const std::vector<uint8_t>& blob) {
        if (!h.setChunk(true, blob.data(), blob.size()))
            return "round-trip failed: the plugin refused the captured patch chunk";
        return "round-trip ok (the plugin reloads the captured patch)";
    }
};

std::string findPlugin() {
    const char* candidates[] = {
        "C:\\Program Files (x86)\\Steinberg\\VstPlugins\\Hypersonic2.dll",
        "C:\\Program Files (x86)\\Steinberg\\VstPlugins\\Hypersonic 2.dll",
        "C:\\Program Files (x86)\\Steinberg\\VstPlugins\\Hypersonic2\\Hypersonic2.dll",
        "C:\\Program Files (x86)\\VstPlugins\\Hypersonic2.dll",
        "C:\\Program Files (x86)\\VstPlugins\\Hypersonic 2.dll",
        "C:\\Program Files\\Steinberg\\VstPlugins\\Hypersonic2.dll",
        "C:\\Program Files\\VstPlugins\\Hypersonic2.dll",
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string notes = "24-96/3";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--plugin") o.plugin = next();
        else if (a == "--list") o.list = next();
        else if (a == "--out") o.out = next();
        else if (a == "--mode") {
            const std::string m = next();
            o.snapshot = o.record = o.patch = false;
            if (m == "snapshot") o.snapshot = true;
            else if (m == "record") o.record = true;
            else if (m == "patch") o.patch = true;
            else if (m == "both") o.snapshot = o.record = true;
            else if (m == "all") o.snapshot = o.record = o.patch = true;
            else {
                std::fprintf(stderr, "--mode: patch, snapshot, record, both or all\n");
                return 2;
            }
        }
        else if (a == "--part") o.part = std::atoi(next().c_str());
        else if (a == "--notes") notes = next();
        else if (a == "--vels") {
            o.vels.clear();
            for (int v : parseNotes(next())) if (v >= 1) o.vels.push_back(v);
        }
        else if (a == "--hold") o.hold = std::atof(next().c_str());
        else if (a == "--tail") o.tail = std::atof(next().c_str());
        else if (a == "--quiet-db") o.quietDb = std::atof(next().c_str());
        else if (a == "--settle") o.settle = std::atof(next().c_str());
        else if (a == "--speed") o.speed = std::atof(next().c_str());
        else if (a == "--rate") o.sampleRate = std::atof(next().c_str());
        else if (a == "--block") o.block = std::atoi(next().c_str());
        else if (a == "--limit") o.limit = std::atoi(next().c_str());
        else if (a == "--max-fails") o.maxFails = std::atoi(next().c_str());
        else if (a == "--no-gui") o.gui = false;
        else if (a == "--overwrite") o.overwrite = true;
        else { std::fprintf(stderr, "unknown option %s (--help lists them)\n", a.c_str()); return 2; }
    }
    o.notes = parseNotes(notes);
    if (o.list.empty()) { usage(); return 2; }
    if (o.part < 1 || o.part > 16 || o.block < 16 || o.block > 8192 || o.sampleRate < 8000 || o.notes.empty() ||
        o.vels.empty() || o.hold <= 0) {
        std::fprintf(stderr, "bad option value (--part 1-16, --block 16-8192, notes and velocities needed)\n");
        return 2;
    }
    if (o.plugin.empty()) o.plugin = findPlugin();
    if (o.plugin.empty()) {
        std::fprintf(stderr, "Hypersonic 2 not found in the usual folders: give it with --plugin PATH\n");
        return 2;
    }
    std::vector<Patch> list = readList(o.list);
    if (o.limit > 0 && size_t(o.limit) < list.size()) list.resize(size_t(o.limit));
    if (list.empty()) { std::fprintf(stderr, "no patches in %s\n", o.list.c_str()); return 2; }

#if defined(_WIN32)
    OleInitialize(nullptr);
#endif
    Host h;
    h.opt = o;
    gHost = &h;
#if defined(_WIN32)
    // The watchdog starts before the plugin is loaded: a wedged licence check hangs in LoadLibrary / effOpen,
    // before any idle call. Nothing in-process recovers from a hang, so exit; the outer capture_all.ps1
    // continues with a fresh process. Patch, snapshot and record modes all idle the plugin every block, so no
    // healthy run trips it (the content scan gets a longer budget).
    h.progress();
    {
        static Host* hostPtr = &h;
        std::thread watchdog([] {
            const auto stuckFor = std::chrono::seconds(45);
            const auto stuckLoading = std::chrono::seconds(120);  // open + licence + content scan
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                Host& host = *hostPtr;
                const auto limit = host.fx != nullptr ? stuckFor : stuckLoading;
                if (std::chrono::steady_clock::now() - host.lastProgress > limit) {
                    std::printf("\nthe plugin stopped responding (wedged): exiting this process; run "
                                "hs2capture / capture_all.ps1 again to continue\n");
                    std::fflush(stdout);
                    std::exit(2);
                }
            }
        });
        watchdog.detach();
    }
#endif
    if (!h.load(o.plugin)) return 1;
    Capture cap(h);

#if defined(_WIN32)
    PatchHook hook;
    if (o.patch) {
        // gHook first: once install() swaps the import, any of the plugin's threads can call hookedDelete, which
        // reaches the real operator delete through gHook.
        gHook = &hook;
        if (!hook.install(reinterpret_cast<uintptr_t>(h.lib))) {
            gHook = nullptr;
            std::fprintf(stderr,
                         "cannot hook the plugin's operator delete import: this Hypersonic.dll is not the "
                         "expected build\n");
            h.close();
            return 1;
        }
        cap.hook = &hook;
        std::printf("patch capture on: the patch the plugin loads is read as the plugin frees its buffer\n");
    }
#else
    if (o.patch) {
        std::fprintf(stderr, "--mode patch needs the Windows plugin (Hypersonic.dll)\n");
        return 2;
    }
#endif

    const fs::path out = o.out;
    fs::create_directories(out / "snapshots");
    if (o.record) fs::create_directories(out / "samples");
    if (o.patch) fs::create_directories(out / "patches");
    const fs::path resultsPath = out / "results.tsv";
    const bool newResults = !fs::exists(resultsPath);
    std::ofstream results(resultsPath, std::ios::app);
    if (newResults) results << "bank\tindex\tname\tstatus\tdetail\n";

    // Let the plugin finish starting up (content scan) before the first program change.
    for (int b = 0; b < int(2.0 * o.sampleRate / o.block); ++b) {
        h.process({}, nullptr, nullptr);
        h.pump();
    }

    int done = 0, failed = 0, skipped = 0, consecutiveFails = 0;
    for (size_t k = 0; k < list.size(); ++k) {
        const Patch& p = list[k];
        char id[16];
        std::snprintf(id, sizeof(id), "%03d-%03d", p.bank, p.index);
        const std::string base = std::string(id) + " " + safeName(p.name);
        const fs::path fxb = out / "snapshots" / (base + ".fxb"), fxp = out / "snapshots" / (base + ".fxp");
        const fs::path sdir = out / "samples" / base;
        const fs::path stss = out / "patches" / (base + ".stss");
        const bool haveSnap = fs::exists(fxb), haveRec = fs::exists(sdir / (base + ".sfz")),
                   havePatch = fs::exists(stss);
        if (!o.overwrite && (!o.snapshot || haveSnap) && (!o.record || haveRec) && (!o.patch || havePatch)) {
            ++skipped;
            continue;
        }
        std::printf("[%zu/%zu] bank %d patch %d %s: loading\n", k + 1, list.size(), p.bank, p.index, p.name.c_str());
        std::fflush(stdout);
        std::string why;
        if (cap.hook) {
            cap.hookMark = cap.hook->count();
            cap.hook->wantName(p.name);  // the hook keeps only this patch's loads
        }
        const size_t mark = cap.hookMark;
        const bool loaded = cap.loadPatch(p, why);
        std::string detail = why;
        if (o.patch) {
            std::string pd;
            std::vector<uint8_t> blob = cap.capturePatch(mark, pd);
            if (blob.empty()) {
                std::printf("    FAILED: %s\n", pd.c_str());
                results << p.bank << '\t' << p.index << '\t' << p.name << "\tfailed\t" << pd
                        << (loaded && !why.empty() ? "; " + why : "") << "\n";
                results.flush();
                ++failed;
                if (++consecutiveFails >= o.maxFails && o.maxFails > 0) {
                    std::printf("stopping: %d consecutive failed loads (a wedged plugin fails every patch after "
                                "one); run hs2capture again to continue with a fresh plugin\n",
                                consecutiveFails);
                    goto out;
                }
                continue;
            }
            consecutiveFails = 0;
            if (!pd.empty()) {
                std::printf("    warning: %s\n", pd.c_str());
                detail += (detail.empty() ? "" : "; ") + pd;
            }
            {
                std::ofstream f(stss, std::ios::binary);
                f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
            }
            const std::string v = cap.verify(blob);
            std::printf("    patch: %zu bytes; %s\n", blob.size(), v.c_str());
            detail += (detail.empty() ? "" : "; ") + v;
            const int bad = Capture::outOfRange(blob);
            if (bad != 0) {
                std::printf("    %d patch parameters outside 0..1\n", bad);
                detail += "; " + std::to_string(bad) + " patch parameters outside 0..1";
            }
        }
        if (!loaded) {
            if (!o.patch) {
                std::printf("    FAILED: %s\n", why.c_str());
                results << p.bank << '\t' << p.index << '\t' << p.name << "\tfailed\t" << why << "\n";
                results.flush();
                ++failed;
                if (++consecutiveFails >= o.maxFails && o.maxFails > 0) {
                    std::printf("stopping: %d consecutive failed loads (a wedged plugin fails every patch after "
                                "one); run hs2capture again to continue with a fresh plugin\n",
                                consecutiveFails);
                    goto out;
                }
                continue;
            }
            // The patch itself was captured and verified; only the state did not settle (e.g. the same patch
            // reloaded), so there is nothing to snapshot or record.
            consecutiveFails = 0;
            results << p.bank << '\t' << p.index << '\t' << p.name << "\tok\t" << detail << "\n";
            results.flush();
            ++done;
            continue;
        }
        consecutiveFails = 0;
        if (!why.empty()) std::printf("    warning: %s\n", why.c_str());
        if (o.snapshot) {
            const std::vector<uint8_t> bank = h.chunk(false), prog = h.chunk(true);
            if (bank.empty()) detail += (detail.empty() ? "" : "; ") + std::string("no state chunk");
            else writeFx(fxb, h.fx, bank, true, p.name);
            if (!prog.empty()) writeFx(fxp, h.fx, prog, false, p.name);
            std::printf("    snapshot: %zu bytes (.fxb), %zu bytes (.fxp)\n", bank.size(), prog.size());
        }
        if (o.record) {
            std::printf("    recording %zu notes x %zu velocities\n", o.notes.size(), o.vels.size());
            std::fflush(stdout);
            const int silent = cap.record(p, sdir, base);
            if (silent) {
                detail += (detail.empty() ? "" : "; ") + std::to_string(silent) + " silent notes skipped";
                std::printf("    %d notes were silent (skipped)\n", silent);
            }
        }
        results << p.bank << '\t' << p.index << '\t' << p.name << "\tok\t" << detail << "\n";
        results.flush();
        ++done;
    }
out:
    std::printf("done: %d captured, %d failed, %d already there (skipped). Results: %s\n", done, failed, skipped,
                resultsPath.string().c_str());
#if defined(_WIN32)
    if (gHook != nullptr) {
        gHook->uninstall();
        gHook = nullptr;
    }
#endif
    // Close gracefully: the original leaves the licence / content state cleaner when the current part is
    // loaded with a plain factory patch (not a copy-protected capture, whose plain chunk was just set) and
    // the plugin has idled for a moment. Wedged sessions crashed at close and left later ones worse.
    if (cap.hook) cap.hook->wantName("-nothing-");
    {
        Patch gm;
        gm.bank = 8;
        gm.index = 28;
        gm.name = "GM Electric Piano 2";
        std::string why;
        cap.loadPatch(gm, why);
        for (int b = 0; b < int(1.0 * o.sampleRate / o.block); ++b) {
            h.process({}, nullptr, nullptr);
            h.pump();
        }
    }
    h.close();
    return failed ? 3 : 0;
}
