// Host test for HS2 MIDI Player: loads the desktop build (hs2-midi-player/build.sh --native) through the plugin API
// as Schwung's shim loads a tool's DSP, with a fake Move clock, against make_test_files.py's content and MIDI files.
// Covers: content scan, opening files (and a broken one), the read-ahead of program changes (GM mapping, the GM drum
// kit on channel 10, a program change in the middle of the file), playing at the file's tempo map, bar and tempo
// position, seeking, looping, following Move's clock, mute / solo, USB MIDI on any channel with program changes,
// patches chosen by hand (locked), all 16 channels at once, GM off, settings kept in settings.json.
//
//   hs2-midi-player/tests/run_tests.sh
#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "plugin_api_v1.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) {                                                   \
            ++g_pass;                                                 \
        } else {                                                      \
            ++g_fail;                                                 \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                        \
            std::fprintf(stderr, "\n");                               \
        }                                                             \
    } while (0)

static double g_beat = -1;
static bool g_running = false;
static const double kBpm = 100;
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
        static char buf[65536];
        const int n = api->get_param(inst, k.c_str(), buf, sizeof(buf));
        return n < 0 ? std::string("<none>") : std::string(buf, size_t(n));
    }
    void set(const std::string& k, const std::string& v) { api->set_param(inst, k.c_str(), v.c_str()); }
    void midi(uint8_t a, uint8_t b, uint8_t c, int source = MOVE_MIDI_SOURCE_EXTERNAL) {
        const uint8_t m[3] = {a, b, c};
        api->on_midi(inst, m, 3, source);
    }
    double render(int blocks) {  // RMS of the blocks; Move's clock runs along
        int16_t buf[256];
        double sum = 0;
        for (int b = 0; b < blocks; ++b) {
            api->render_block(inst, buf, 128);
            for (int i = 0; i < 256; ++i) sum += double(buf[i]) * buf[i];
            if (g_running) g_beat += 128.0 / 44100.0 * kBpm / 60.0;
        }
        return std::sqrt(sum / (blocks * 256.0)) / 32768.0;
    }
    double seconds(double s) { return render(int(s * 44100 / 128 + 0.5)); }
    bool waitFor(const std::function<bool()>& ok, int ms = 10000) {
        for (int t = 0; t < ms / 3; ++t) {
            if (ok()) return true;
            render(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return ok();
    }
    // a number from a flat JSON object
    double num(const std::string& json, const std::string& key) {
        const auto k = json.find("\"" + key + "\":");
        return k == std::string::npos ? -1e9 : std::atof(json.c_str() + k + key.size() + 3);
    }
    double pos(const std::string& key) { return num(get("pos"), key); }
    // part p's object in the parts JSON
    std::string part(int p) {
        const std::string j = get("parts");
        size_t at = 0;
        for (int i = 0; i <= p; ++i) {
            at = j.find("{\"name\"", at);
            if (at == std::string::npos) return "";
            if (i < p) ++at;
        }
        return j.substr(at, j.find('}', at) - at + 1);
    }
    bool partIs(int p, const std::string& name) { return part(p).find("\"name\":\"" + name + "\"") != std::string::npos; }
    bool prepared() { return pos("prep") == 0; }
};

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: host_test <hs2mp.so> <test folder (make_test_files.py)> <module folder>\n");
        return 2;
    }
    const std::string root = argv[2], moduleDir = argv[3];
    std::filesystem::remove(std::filesystem::path(moduleDir) / "settings.json");
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
        Mod m{api, api->create_instance("/nonexistent/module", nullptr)};
        CHECK(m.waitFor([&] { return m.get("ready") == "-1"; }, 5000), "missing content reports ready=-1");
        CHECK(m.get("status").find("not found") != std::string::npos, "status: %s", m.get("status").c_str());
        m.set("transport", "play");  // no crash
        CHECK(m.render(5) == 0.0, "silent without content");
        api->destroy_instance(m.inst);
    }

    setenv("HS2_CONTENT", (root + "/content").c_str(), 1);
    Mod m{api, api->create_instance(moduleDir.c_str(), nullptr)};
    CHECK(m.waitFor([&] { return m.get("ready") == "1"; }), "content scanned: %s", m.get("status").c_str());
    CHECK(m.get("banks").find("079 GM Drums") != std::string::npos, "banks %s", m.get("banks").c_str());
    CHECK(m.get("bankp:1").find("\"GM 8\"") != std::string::npos, "bank 078 patches %s", m.get("bankp:1").c_str());
    CHECK(m.get("song").find("\"loaded\":0") != std::string::npos, "no file yet");
    CHECK(m.get("cfg").find("\"gm\":1") != std::string::npos, "GM on by default: %s", m.get("cfg").c_str());

    // ---- a broken file
    m.set("file_path", root + "/midi/broken.mid");
    CHECK(m.waitFor([&] { return m.get("song").find("not a MIDI file") != std::string::npos; }), "broken file: %s",
          m.get("song").c_str());

    // ---- open the song: read ahead, prepared patches
    m.set("file_path", root + "/midi/song.mid");
    CHECK(m.waitFor([&] { return m.get("song").find("\"loaded\":1") != std::string::npos && m.prepared(); }),
          "song opened: %s / %s", m.get("song").c_str(), m.get("pos").c_str());
    const std::string sj = m.get("song");
    CHECK(sj.find("\"tracks\":3") != std::string::npos && sj.find("\"bars\":4") != std::string::npos &&
              sj.find("\"title\":\"Test Song\"") != std::string::npos && sj.find("\"parts\":3") != std::string::npos,
          "song info %s", sj.c_str());
    CHECK(std::fabs(m.num(sj, "len") - 12.0) < 0.05, "length 12 s (two tempos): %s", sj.c_str());
    CHECK(m.num(sj, "patches") == 4, "four patches asked for (GM 4, kit, GM 6, GM 7): %s", sj.c_str());
    m.render(2);
    CHECK(m.partIs(0, "GM 4"), "part 1 starts on its program change (GM 4): %s", m.part(0).c_str());
    CHECK(m.partIs(9, "Standard Kit"), "channel 10 plays the GM kit: %s", m.part(9).c_str());
    CHECK(m.partIs(1, "GM 6"), "part 2 starts on GM 6: %s", m.part(1).c_str());
    CHECK(m.part(4).find("\"used\":0") != std::string::npos && m.partIs(4, "empty"), "part 5 unused: %s",
          m.part(4).c_str());
    CHECK(m.pos("bar") == 1 && m.pos("bpm") == 120, "at bar 1, 120 BPM: %s", m.get("pos").c_str());

    // ---- play at the file's tempo
    m.set("transport", "play");
    const double rms = m.seconds(1.0);
    CHECK(rms > 1e-4, "plays (rms %.6f)", rms);
    CHECK(std::fabs(m.pos("sec") - 1.0) < 0.02 && m.pos("bar") == 1 && m.pos("beat") == 3, "1 s = bar 1 beat 3: %s",
          m.get("pos").c_str());
    m.seconds(1.5);  // into bar 2: part 2's second program change
    CHECK(m.pos("bar") == 2, "bar 2: %s", m.get("pos").c_str());
    CHECK(m.partIs(1, "GM 7"), "part 2 changed patch in the same block (GM 7): %s", m.part(1).c_str());
    m.seconds(2.0);  // bar 3: 60 BPM
    CHECK(m.pos("bar") == 3 && m.pos("bpm") == 60, "bar 3 at 60 BPM: %s", m.get("pos").c_str());
    CHECK(m.part(9).find("\"notes\":0") == std::string::npos, "the drums played: %s", m.part(9).c_str());

    // ---- seek
    m.set("seek_bar", "2");
    m.render(1);
    CHECK(m.pos("bar") == 2 && std::fabs(m.pos("sec") - 2.0) < 0.01, "seek to bar 2: %s", m.get("pos").c_str());
    CHECK(m.partIs(1, "GM 7"), "the chase keeps part 2's program at bar 2: %s", m.part(1).c_str());
    m.set("seek_bar", "1");
    m.render(1);
    CHECK(m.partIs(1, "GM 6"), "back at bar 1, part 2 is GM 6 again: %s", m.part(1).c_str());
    m.set("step_bar", "3");
    m.render(1);
    CHECK(m.pos("bar") == 4, "three bars on: %s", m.get("pos").c_str());
    m.set("step_bar", "-1");
    m.render(1);
    CHECK(m.pos("bar") == 3, "one back: %s", m.get("pos").c_str());

    // ---- the end, and looping
    m.set("seek_bar", "4");
    m.seconds(4.2);
    CHECK(m.pos("play") == 0 && m.pos("end") == 1, "stops at the end: %s", m.get("pos").c_str());
    m.set("cfg:loop", "1");
    m.set("transport", "play");  // from the end: starts again
    m.render(2);
    CHECK(m.pos("play") == 1 && m.pos("bar") == 1, "play at the end starts over: %s", m.get("pos").c_str());
    m.set("seek_bar", "4");
    m.seconds(4.3);
    CHECK(m.pos("play") == 1 && m.pos("bar") == 1, "loops: %s", m.get("pos").c_str());
    m.set("cfg:loop", "0");
    m.set("transport", "stop");
    m.render(2);
    CHECK(m.pos("play") == 0, "stopped");

    // ---- mute and solo
    m.set("seek_bar", "1");
    m.set("solo:4", "1");  // an unused part: everything else is muted
    m.set("transport", "play");
    const double soloQuiet = m.seconds(0.5);
    CHECK(soloQuiet < 1e-5, "soloing an unused part silences the rest (rms %.6f)", soloQuiet);
    m.set("solo:4", "0");
    const double unsolo = m.seconds(0.6);
    CHECK(unsolo > 1e-4, "unsoloed (rms %.6f)", unsolo);
    m.set("mute:0", "toggle");
    CHECK(m.part(0).find("\"mute\":1") != std::string::npos, "part 1 muted");
    m.set("mute:0", "toggle");
    m.set("transport", "stop");

    // ---- a patch chosen by hand is locked against the file's program changes
    m.set("load:1", "0.1");  // part 2: Test Pad
    CHECK(m.waitFor([&] { return m.partIs(1, "Test Pad"); }), "part 2 loaded by hand: %s", m.part(1).c_str());
    CHECK(m.part(1).find("\"lock\":1") != std::string::npos, "and locked");
    m.set("seek_bar", "2");
    m.set("transport", "play");
    m.seconds(0.3);
    CHECK(m.partIs(1, "Test Pad"), "the file's program change leaves a locked part alone: %s", m.part(1).c_str());
    m.set("transport", "stop");
    m.set("lock:1", "0");
    m.set("seek_bar", "2");
    m.render(1);
    CHECK(m.partIs(1, "GM 7"), "unlocked, the chase brings the file's patch back: %s", m.part(1).c_str());

    // ---- level and pan
    m.set("level:0", "0.5");
    CHECK(std::fabs(m.num(m.part(0), "level") - 0.5) < 1e-3, "level %s", m.part(0).c_str());
    m.set("level:0", "+10");
    CHECK(std::fabs(m.num(m.part(0), "level") - 0.6) < 1e-3, "level step %s", m.part(0).c_str());
    m.set("pan:0", "-20");
    CHECK(m.num(m.part(0), "pan") < 0.5, "pan step %s", m.part(0).c_str());

    // ---- USB MIDI: any channel, program changes load (not prepared: on the worker)
    m.midi(0xc4, 2, 0);  // channel 5, GM program 2 = GM 3
    CHECK(m.waitFor([&] { return m.partIs(4, "GM 3"); }), "USB program change loads part 5: %s", m.part(4).c_str());
    m.midi(0x94, 60, 100);
    const double usb = m.seconds(0.3);
    m.midi(0x84, 60, 0);
    CHECK(usb > 1e-4, "USB notes play part 5 (rms %.6f)", usb);
    m.seconds(1.5);
    m.midi(0x94, 60, 100, MOVE_MIDI_SOURCE_INTERNAL);  // Move's own pads are the UI's
    CHECK(m.seconds(0.2) < 1e-5, "internal MIDI is not played");
    m.set("cfg:usb", "0");
    m.midi(0x94, 60, 100);
    CHECK(m.seconds(0.2) < 1e-5, "USB input off");
    m.set("cfg:usb", "1");

    // ---- following Move's clock
    m.set("cfg:tempo", "1");
    m.set("seek_bar", "1");
    m.render(2);
    CHECK(m.pos("play") == 0, "sync: waits for Move's transport");
    g_beat = 0;
    g_running = true;
    m.render(2);
    CHECK(m.pos("play") == 1, "sync: Move's Play starts it: %s", m.get("pos").c_str());
    m.seconds(60.0 / kBpm * 5);  // five beats at Move's 100 BPM
    CHECK(m.pos("bar") == 2 && m.pos("beat") == 2 && m.pos("bpm") == 100, "sync: bar 2 beat 2 at Move's tempo: %s",
          m.get("pos").c_str());
    g_running = false;
    m.render(2);
    CHECK(m.pos("play") == 0, "sync: Move's Stop stops it");
    g_beat = 2.5;  // Move starts off its bar grid: the file waits for the next bar line
    g_running = true;
    m.render(1);
    CHECK(m.pos("wait") == 1, "sync: waits for Move's next bar: %s", m.get("pos").c_str());
    g_running = false;
    m.render(2);
    m.set("cfg:tempo", "0");

    // ---- sixteen channels
    m.set("file_path", root + "/midi/sixteen.mid");
    CHECK(m.waitFor([&] { return m.get("song").find("sixteen.mid") != std::string::npos && m.prepared(); }),
          "sixteen.mid opened");
    m.render(2);
    int used = 0;
    for (int p = 0; p < 16; ++p) used += m.part(p).find("\"used\":1") != std::string::npos;
    CHECK(used == 16, "16 parts used, got %d", used);
    m.set("transport", "play");
    m.seconds(0.5);
    int sounding = 0;
    for (int p = 0; p < 16; ++p) sounding += m.num(m.part(p), "voices") > 0;
    CHECK(sounding == 16, "16 parts sound at once, got %d", sounding);
    const std::string cpu = m.get("cpu");
    CHECK(cpu.find(',') != std::string::npos && std::atoi(cpu.c_str()) >= 0, "cpu %s", cpu.c_str());
    m.set("transport", "stop");

    // ---- GM off: the file's programs come from the normal banks
    m.set("file_path", root + "/midi/song.mid");
    CHECK(m.waitFor([&] { return m.get("song").find("song.mid") != std::string::npos && m.prepared(); }), "song again");
    m.set("cfg:gm", "0");
    CHECK(m.waitFor([&] { return m.prepared() && m.part(0).find("\"missing\":\"missing 0:4\"") != std::string::npos; }),
          "GM off: program 4 of bank 0 is missing: %s", m.part(0).c_str());
    m.set("cfg:gm", "1");
    CHECK(m.waitFor([&] { return m.prepared() && m.partIs(0, "GM 4"); }), "GM on again: %s", m.part(0).c_str());

    // ---- settings kept in the module folder
    m.set("cfg:loop", "1");
    m.set("lastdir", "/data/UserData/UserLibrary/MIDI");
    {
        std::ifstream f(std::filesystem::path(moduleDir) / "settings.json");
        const std::string j((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(j.find("\"loop\":1") != std::string::npos && j.find("UserLibrary/MIDI") != std::string::npos,
              "settings.json %s", j.c_str());
    }
    api->destroy_instance(m.inst);
    Mod m2{api, api->create_instance(moduleDir.c_str(), nullptr)};
    CHECK(m2.waitFor([&] { return m2.get("ready") == "1"; }), "second instance");
    CHECK(m2.get("cfg").find("\"loop\":1") != std::string::npos, "settings restored: %s", m2.get("cfg").c_str());
    api->destroy_instance(m2.inst);

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
