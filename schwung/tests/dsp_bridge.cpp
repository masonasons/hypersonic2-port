// A stand-in Schwung host for the UI test (ui_test.mjs): loads the desktop module build, renders blocks in real time
// on its own thread (with a running fake Move clock), and answers the UI's get / set calls over two FIFOs, one line
// per request:  g <key>  ->  value (or \x01 when the module has no such key);  s <key>\t<value>  ->  ok;
//               m <status> <d1> <d2>  ->  ok (MIDI from Move's tracks);  q  -> quit.
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "plugin_api_v1.h"

static std::atomic<double> g_beat{0};
static void hostLog(const char* m) {
    if (std::getenv("HS2_VERBOSE")) std::fprintf(stderr, "[log] %s\n", m);
}
static int clockStatus() { return MOVE_CLOCK_STATUS_RUNNING; }
static float bpm() { return 120.f; }
static double beatPos() { return g_beat.load(); }

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    setenv("HS2_CONTENT", argv[2], 1);
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    auto init = reinterpret_cast<move_plugin_init_v2_fn>(dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL));
    host_api_v1_t host{};
    host.api_version = 1;
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    host.log = hostLog;
    host.get_clock_status = clockStatus;
    host.get_bpm = bpm;
    host.get_beat_position = beatPos;
    plugin_api_v2_t* api = init(&host);
    void* inst = api->create_instance("/tmp", nullptr);
    std::atomic<bool> run{true};
    std::thread audio([&] {
        int16_t buf[256];
        while (run) {
            api->render_block(inst, buf, 128);
            g_beat = g_beat.load() + 128.0 / 44100.0 * 2.0;
            std::this_thread::sleep_for(std::chrono::microseconds(2900));
        }
    });
    FILE* in = std::fopen(argv[3], "r");
    FILE* out = std::fopen(argv[4], "w");
    static char line[70000], buf[65536];
    while (in && out && std::fgets(line, sizeof(line), in)) {
        line[std::strcspn(line, "\n")] = 0;
        if (line[0] == 'q') break;
        if (line[0] == 'g') {
            const int n = api->get_param(inst, line + 2, buf, sizeof(buf));
            if (n < 0) std::fputs("\x01", out);
            else std::fwrite(buf, 1, size_t(n), out);
        } else if (line[0] == 's') {
            char* tab = std::strchr(line + 2, '\t');
            if (tab) *tab = 0;
            api->set_param(inst, line + 2, tab ? tab + 1 : "");
            std::fputs("ok", out);
        } else if (line[0] == 'm') {
            unsigned a = 0, b = 0, c = 0;
            std::sscanf(line + 2, "%u %u %u", &a, &b, &c);
            const uint8_t msg[3] = {uint8_t(a), uint8_t(b), uint8_t(c)};
            api->on_midi(inst, msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
            std::fputs("ok", out);
        }
        std::fputc('\n', out);
        std::fflush(out);
    }
    run = false;
    audio.join();
    api->destroy_instance(inst);
    return 0;
}
