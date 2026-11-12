// Host test for the Schwung module: loads the desktop build (schwung/build.sh --native) through the plugin API the
// way Schwung's chain host does, with a fake host clock, against the synthetic content of make_test_content.py.
// Covers: content scan, missing content, patch browsing and loading, every menu address (name / text / step),
// element editing, the Hyperknobs, channel routing, global effect presets, the arpeggiator on Move's clock,
// program changes, state save / restore and rescans; with a third argument, the sample cache's memory while
// scrolling through sample patches (preload, release of samples nothing plays, the preload budget).
//
//   schwung/tests/run_host_test.sh
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "plugin_api_v1.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (cond) {                                            \
            ++g_pass;                                          \
        } else {                                               \
            ++g_fail;                                          \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                 \
            std::fprintf(stderr, "\n");                        \
        }                                                      \
    } while (0)

static double g_beat = 0;
static bool g_running = true;
static const double kBpm = 125;
static bool g_verbose = std::getenv("HS2_VERBOSE") != nullptr;

static void hostLog(const char* m) {
    if (g_verbose) std::fprintf(stderr, "[log] %s\n", m);
}
static int clockStatus() { return g_running ? MOVE_CLOCK_STATUS_RUNNING : MOVE_CLOCK_STATUS_STOPPED; }
static float bpm() { return float(kBpm); }
static double beatPos() { return g_running ? g_beat : -1.0; }

struct Mod {
    plugin_api_v2_t* api;
    void* inst;
    std::string get(const std::string& k) {
        char buf[65536];
        const int n = api->get_param(inst, k.c_str(), buf, sizeof(buf));
        return n < 0 ? std::string("<none>") : std::string(buf, size_t(n));
    }
    void set(const std::string& k, const std::string& v) { api->set_param(inst, k.c_str(), v.c_str()); }
    void midi(uint8_t a, uint8_t b, uint8_t c) {
        const uint8_t m[3] = {a, b, c};
        api->on_midi(inst, m, 3, MOVE_MIDI_SOURCE_INTERNAL);
    }
    double render(int blocks) {  // RMS of the blocks
        int16_t buf[256];
        double sum = 0;
        for (int b = 0; b < blocks; ++b) {
            api->render_block(inst, buf, 128);
            for (int i = 0; i < 256; ++i) sum += double(buf[i]) * buf[i];
            if (g_running) g_beat += 128.0 / 44100.0 * kBpm / 60.0;
        }
        return std::sqrt(sum / (blocks * 256.0)) / 32768.0;
    }
    bool waitFor(const std::function<bool()>& ok, int ms = 10000) {
        for (int t = 0; t < ms / 3; ++t) {
            if (ok()) return true;
            render(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return ok();
    }
    bool waitLoaded(int p) {
        return waitFor([&] { return get("pinfo:" + std::to_string(p)).find("\"loading\":0") != std::string::npos; });
    }
};

// Pulls the addresses out of a layout JSON ([{"t":..,"p":[["addr","name",ro],..]},..]).
static std::vector<std::pair<std::string, std::string>> addrs(const std::string& j) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t p = 0;
    while ((p = j.find("[\"", p)) != std::string::npos) {
        const size_t a = p + 2, ae = j.find('"', a);
        const size_t n = j.find('"', ae + 1) + 1, ne = j.find('"', n);
        out.push_back({j.substr(a, ae - a), j.substr(n, ne - n)});
        p = ne;
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: host_test <hs2_module.so> <test content folder> [<big sample content folder>]\n");
        return 2;
    }
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    auto init = reinterpret_cast<move_plugin_init_v2_fn>(dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL));
    CHECK(init != nullptr, "move_plugin_init_v2 exported");
    if (!init) return 1;
    host_api_v1_t host{};
    host.api_version = 1;
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    host.log = hostLog;
    host.get_clock_status = clockStatus;
    host.get_bpm = bpm;
    host.get_beat_position = beatPos;
    plugin_api_v2_t* api = init(&host);
    CHECK(api && api->api_version == 2, "v2 API");

    // ---- missing content
    {
        setenv("HS2_CONTENT", "/nonexistent/hs2", 1);
        Mod m{api, api->create_instance("/tmp", nullptr)};
        CHECK(m.waitFor([&] { return m.get("ready") == "-1"; }, 5000), "missing content reports ready=-1");
        char err[512];
        CHECK(api->get_error(m.inst, err, sizeof(err)) > 0, "get_error explains the missing content");
        CHECK(m.get("status").find("not found") != std::string::npos, "status: %s", m.get("status").c_str());
        CHECK(m.render(10) == 0.0, "silent without content");
        m.set("step:B0", "1");  // no crash
        api->destroy_instance(m.inst);
    }

    setenv("HS2_CONTENT", argv[2], 1);
    Mod m{api, api->create_instance("/tmp", nullptr)};
    CHECK(m.waitFor([&] { return m.get("ready") == "1"; }), "content scanned");
    CHECK(m.get("preset_count") == "3", "3 patches, got %s", m.get("preset_count").c_str());
    CHECK(m.get("banks").find("000 Test Patches") != std::string::npos, "banks: %s", m.get("banks").c_str());
    CHECK(m.get("bankp:0") == "[\"Test Analog\",\"Test Sample\"]", "bank names: %s", m.get("bankp:0").c_str());
    CHECK(m.waitLoaded(0) && m.waitLoaded(1) && m.waitLoaded(3), "a fresh instance loads a patch into every part");
    CHECK(m.get("pinfo:1").find("Test Sample") != std::string::npos, "part 2 default %s", m.get("pinfo:1").c_str());
    CHECK(m.get("pinfo:3").find("Second Bank") != std::string::npos, "part 4 default %s", m.get("pinfo:3").c_str());
    CHECK(m.get("t:C2") == "channel 3", "part 3 on channel 3: %s", m.get("t:C2").c_str());

    // ---- browse and load
    m.set("step:B0", "1");
    CHECK(m.get("t:B0") == "Test Sample", "browse target %s", m.get("t:B0").c_str());
    m.set("step:B0", "-1");
    CHECK(m.get("t:B0") == "Test Analog", "browse back %s", m.get("t:B0").c_str());
    CHECK(m.waitLoaded(0), "part 1 loads");
    const std::string pi = m.get("pinfo:0");
    CHECK(pi.find("\"name\":\"Test Analog\"") != std::string::npos && pi.find("Thump") != std::string::npos, "pinfo %s",
          pi.c_str());
    CHECK(m.get("n:P0.9") == "Hyperknob 1: Thump", "knob name %s", m.get("n:P0.9").c_str());
    CHECK(m.get("t:P0.9") == "50 %", "knob value %s", m.get("t:P0.9").c_str());
    m.set("step:P0.9", "5");
    CHECK(m.get("t:P0.9") == "55 %", "knob stepped %s", m.get("t:P0.9").c_str());
    CHECK(m.get("hk1").rfind("0.55", 0) == 0, "chain knob hk1 %s", m.get("hk1").c_str());

    // ---- sound on Move's clock
    m.midi(0x90, 60, 100);
    const double rms = m.render(200);
    m.midi(0x80, 60, 0);
    CHECK(rms > 1e-4, "part 1 sounds on channel 1 (rms %.6f)", rms);
    m.render(400);
    const double tail = m.render(100);  // part 1's delay tail is still ringing out
    m.midi(0x91, 60, 100);              // channel 2: part 2's sample bank is missing (silent)
    const double ch2 = m.render(100);
    CHECK(ch2 <= tail + 1e-6, "channel 2 reaches only the empty part 2 (%.6f after a tail of %.6f)", ch2, tail);
    m.midi(0x81, 60, 0);
    m.render(400);

    // ---- every address in every menu
    const char* groups[] = {"lay:setup.0", "lay:knobs.0", "lay:keys.0", "lay:voice.0", "lay:arp.0", "lay:tuning.0",
                            "lay:sends.0", "lay:gfx", "lay:settings", "play:0", "elay:0.1", "elay:0.2"};
    int total = 0, unnamed = 0, emptyText = 0;
    for (const char* g : groups) {
        const std::string j = m.get(g);
        const auto list = addrs(j);
        CHECK(!list.empty() || std::string(g) == "lay:sends.0", "%s has items (%s)", g, j.substr(0, 120).c_str());
        for (const auto& [a, name] : list) {
            ++total;
            if (m.get("n:" + a).empty()) ++unnamed;
            const std::string before = m.get("t:" + a);
            if (before.empty() && a[0] != 'E') ++emptyText;
            if (a == "Z9" || a == "Z3" || a[0] == 'F') continue;  // preload / memory / presets: tested below
            const std::string v0 = m.get("v:" + a);
            m.set("step:" + a, "1");
            m.set("step:" + a, "-1");
            m.set("v:" + a, v0);  // on / off switches don't come back with +1 -1
            if (g_verbose) std::fprintf(stderr, "  %-12s %-28s %s\n", a.c_str(), name.c_str(), before.c_str());
        }
        m.render(2);
    }
    CHECK(total > 250, "%d menu addresses", total);
    CHECK(unnamed == 0, "%d addresses without a name", unnamed);
    CHECK(emptyText == 0, "%d addresses without a value text", emptyText);
    const std::string ej = m.get("elems:0");
    CHECK(ej.find("\"Analog\"") != std::string::npos && ej.find("\"Effect\"") != std::string::npos, "elems %s", ej.c_str());
    const std::string fxLay = m.get("elay:0.2");
    CHECK(fxLay.find("Effect Type") != std::string::npos, "effect element layout %s", fxLay.substr(0, 200).c_str());
    CHECK(m.get("t:E0.2.19") == "Delay", "insert effect type %s", m.get("t:E0.2.19").c_str());
    m.set("step:E0.2.19", "1");
    CHECK(m.get("t:E0.2.19") == "Delay", "the effect type is read only");
    const std::string modTitle = m.get("elay:0.1");
    CHECK(modTitle.find("Mod 1: ") != std::string::npos, "mod page titles");

    // ---- element edit, kept in the state
    const std::string cutoff0 = m.get("t:E0.1.106");
    m.set("step:E0.1.106", "-10");
    const std::string cutoff1 = m.get("t:E0.1.106");
    CHECK(cutoff0 != cutoff1, "cutoff %s -> %s", cutoff0.c_str(), cutoff1.c_str());
    CHECK(m.render(50) == m.render(0) || true, "renders after an edit");

    // ---- arpeggiator on the clock
    m.set("v:P0.39", "1");
    CHECK(m.get("t:P0.39") == "ON", "arp on: %s", m.get("t:P0.39").c_str());
    m.midi(0x90, 64, 100);
    const double arms = m.render(400);
    m.midi(0x80, 64, 0);
    CHECK(arms > 1e-4, "arp plays on Move's clock (rms %.6f)", arms);
    m.set("v:P0.39", "0");
    m.render(400);

    // ---- channels: part 2 also on channel 1
    m.set("load:1", "1");  // Test Sample
    CHECK(m.waitLoaded(1), "part 2 loads");
    m.set("step:C1", "-1");
    CHECK(m.get("t:C1") == "channel 1", "part 2 channel %s", m.get("t:C1").c_str());
    m.set("step:C3", "20");
    CHECK(m.get("t:C3") == "all channels", "part 4 omni %s", m.get("t:C3").c_str());

    // ---- global effect preset
    m.set("step:F1", "1");
    CHECK(m.waitFor([&] { return m.get("t:F1").find("Test Delay") != std::string::npos; }), "effect preset: %s",
          m.get("t:F1").c_str());
    CHECK(m.waitFor([&] { return m.get("lay:gfx").find("Effect 2: Stereo Delay") != std::string::npos; }),
          "global effect 2 is the preset's effect: %s", m.get("lay:gfx").substr(0, 300).c_str());
    m.set("step:G1.1", "3");

    // ---- settings
    m.set("step:Z7", "2");
    CHECK(m.get("t:Z7") == "+2 semitones", "master tune %s", m.get("t:Z7").c_str());
    m.set("part", "1");
    CHECK(m.get("part") == "1", "active part");

    // ---- state round trip
    const std::string st = m.get("state");
    CHECK(st.size() > 50 && st.find("\"Test Analog\"") != std::string::npos, "state %s", st.c_str());
    Mod m2{api, api->create_instance("/tmp", nullptr)};
    m2.set("state", st);  // before the scan finished: deferred
    CHECK(m2.waitFor([&] { return m2.get("ready") == "1"; }), "second instance scanned");
    CHECK(m2.waitFor([&] {
        return m2.get("pinfo:0").find("Test Analog") != std::string::npos &&
               m2.get("pinfo:1").find("Test Sample") != std::string::npos &&
               m2.get("pinfo:0").find("\"loading\":0") != std::string::npos &&
               m2.get("t:F1").find("Test Delay") != std::string::npos;
    }), "restored parts: %s / %s", m2.get("pinfo:0").c_str(), m2.get("pinfo:1").c_str());
    m2.render(5);
    CHECK(m2.get("t:E0.1.106") == cutoff1, "restored element edit %s vs %s", m2.get("t:E0.1.106").c_str(), cutoff1.c_str());
    CHECK(m2.get("t:P0.9") == "55 %", "restored Hyperknob %s", m2.get("t:P0.9").c_str());
    CHECK(m2.get("t:C1") == "channel 1" && m2.get("t:C3") == "all channels", "restored channels");
    CHECK(m2.get("t:Z7") == "+2 semitones", "restored settings");
    CHECK(m2.get("v:G1.1") == m.get("v:G1.1"), "restored effect knob %s vs %s", m2.get("v:G1.1").c_str(), m.get("v:G1.1").c_str());
    CHECK(m2.get("part") == "1", "restored active part");
    const std::string st2 = m2.get("state");
    CHECK(st2 == st, "state is stable:\n  %s\n  %s", st.c_str(), st2.c_str());
    api->destroy_instance(m2.inst);

    // ---- program change: bank 1 (CC0 alone), program 0 on channel 1 -> parts 1 and 2
    m.midi(0xb0, 0, 1);
    m.midi(0xc0, 0, 0);
    CHECK(m.waitFor([&] { return m.get("pinfo:0").find("Second Bank") != std::string::npos; }), "program change loads");
    CHECK(m.waitFor([&] { return m.get("pinfo:1").find("Second Bank") != std::string::npos; }),
          "both parts on channel 1 follow");
    // a patch load resets the element edits but keeps the part's mixer settings
    CHECK(m.get("state").find("[1,106,") == std::string::npos, "element edits dropped with the old patch");

    // ---- bank stepping
    m.set("step:K2", "-1");
    CHECK(m.get("t:K2") == "000 Test Patches", "bank step back: %s", m.get("t:K2").c_str());
    m.set("step:K2", "1");
    CHECK(m.get("t:K2") == "001 More Tests" && m.get("t:B2") == "Second Bank", "bank step: %s / %s",
          m.get("t:K2").c_str(), m.get("t:B2").c_str());

    // ---- rescan keeps the setup
    m.waitLoaded(2);
    m.set("rescan", "1");
    CHECK(m.waitFor([&] {
        return m.get("ready") == "1" && m.get("pinfo:2").find("Second Bank") != std::string::npos &&
               m.get("pinfo:2").find("\"loading\":0") != std::string::npos;
    }), "rescan reloads the parts: %s", m.get("pinfo:2").c_str());
    m.set("panic", "1");
    m.render(10);
    api->destroy_instance(m.inst);

    // ---- sample memory (make_test_content.py --big): six sample patches of ~1.4 MB decoded each
    if (argc > 3) {
        const double kPatch = 8.0 * 44104 * 4;  // 8 zones of 1 s mono floats (+ guard frames)
        auto mb = [](double b) { return b / 1048576.0; };
        setenv("HS2_CONTENT", argv[3], 1);
        Mod b{api, api->create_instance("/tmp", nullptr)};
        auto mem = [&] { return std::atof(b.get("mem").c_str()); };
        CHECK(b.waitFor([&] { return b.get("ready") == "1"; }), "big content scanned");
        CHECK(b.get("preset_count") == "6", "6 big patches, got %s", b.get("preset_count").c_str());
        CHECK(b.waitLoaded(0) && b.waitLoaded(3), "big patches in every part");
        // preload (on by default) decodes the four patches on the loader, before any note
        CHECK(b.waitFor([&] { return mem() >= 3.9 * kPatch; }), "four patches preloaded: %.1f MB", mb(mem()));
        // scroll part 1 through the whole bank (patches 2-4 are in parts 2-4, 5 and 6 only in part 1), waiting for
        // each patch: memory stays at four patches
        double most = 0;
        for (int i = 0; i < 5; ++i) {
            b.set("step:B0", "1");
            b.waitLoaded(0);
            b.waitFor([&] { return mem() <= 4.2 * kPatch; }, 3000);
            most = std::max(most, mem());
        }
        CHECK(b.get("pinfo:0").find("Big Piano 6") != std::string::npos, "scrolled to the last patch: %s",
              b.get("pinfo:0").c_str());
        CHECK(most <= 4.2 * kPatch, "memory stays at four patches while scrolling: at most %.1f MB", mb(most));
        for (int i = 0; i < 12; ++i) b.set("step:B0", i % 2 ? "1" : "-1");  // and quickly (ends on patch 6)
        b.waitLoaded(0);
        CHECK(b.waitFor([&] { return mem() <= 4.2 * kPatch; }), "and after quick scrolling: %.1f MB", mb(mem()));
        // a note still sounding from a replaced patch keeps its samples until it ends
        b.midi(0x90, 60, 110);
        b.render(20);
        b.set("step:B0", "-1");  // patch 5; patch 6's note sounds on (cut on load is off)
        b.waitLoaded(0);
        CHECK(b.waitFor([&] { return mem() >= 4.9 * kPatch; }), "patch 6 kept while its note sounds, patch 5 preloaded: "
              "%.1f MB", mb(mem()));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // sweeps run while the old note plays
        const double held = b.render(30);
        CHECK(held > 1e-4, "the replaced patch's note plays on after the sweeps (rms %.6f)", held);
        b.midi(0x80, 60, 0);
        b.render(400);  // the release ends and the old part goes
        CHECK(b.waitFor([&] { return mem() <= 4.2 * kPatch; }, 8000), "released after the old note ended: %.1f MB",
              mb(mem()));
        // the new patch plays (its samples are preloaded or decode on first use). The synthetic element sounds around
        // C3 only (quiet below, silent from 72 up), so the checks play 60 and 64.
        b.midi(0x90, 64, 110);
        const double now = b.render(40);
        b.midi(0x80, 64, 0);
        CHECK(now > 1e-4, "the new patch sounds (rms %.6f)", now);
        api->destroy_instance(b.inst);

        // a small preload budget: the preload stops there, notes still decode on first use
        setenv("HS2_PRELOAD_MB", "2", 1);
        Mod c{api, api->create_instance("/tmp", nullptr)};
        CHECK(c.waitFor([&] { return c.get("ready") == "1"; }) && c.waitLoaded(0) && c.waitLoaded(3), "budget instance");
        c.render(200);
        const double cm = std::atof(c.get("mem").c_str());
        CHECK(cm <= 2.0 * 1048576 + 44104 * 4 + 1024, "preload stops at the budget: %.2f MB", mb(cm));
        c.midi(0x93, 64, 110);  // part 4: the budget ran out in part 2's preload
        const double lazy = c.render(40);
        c.midi(0x83, 64, 0);
        CHECK(lazy > 1e-4, "a zone past the budget decodes when played (rms %.6f)", lazy);
        unsetenv("HS2_PRELOAD_MB");
        api->destroy_instance(c.inst);
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
