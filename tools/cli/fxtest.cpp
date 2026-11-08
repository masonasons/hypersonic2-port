// fxtest: run a ported effect on raw float32 stereo input (as written by tools/emu/fx_oracle.py).
//   fxtest <type> <in.f32> <out.f32> [--params p0,p1,...] [--sr 44100] [--pitch frame:value;frame:value...] [--tempo bpm] [--gated]
// --pitch connects a note-pitch source (Effect::setPitchSource, original vtable +8) and updates it per block.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hs2/effects.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: fxtest <type> <in.f32> <out.f32> [--params a,b,..] [--sr hz]\n");
        return 2;
    }
    const int type = std::atoi(argv[1]);
    std::vector<float> params;
    float sr = 44100.f;
    std::vector<std::pair<int, float>> pitch;
    bool usePitch = false;
    float tempo = 0.f;
    bool gated = false;
    for (int i = 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--params") && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ',')) params.push_back(std::stof(tok));
        } else if (!std::strcmp(argv[i], "--pitch") && i + 1 < argc) {
            usePitch = true;
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';')) {
                const auto c = tok.find(':');
                pitch.emplace_back(std::stoi(tok.substr(0, c)), std::stof(tok.substr(c + 1)));
            }
        } else if (!std::strcmp(argv[i], "--gated")) {
            gated = true;
        } else if (!std::strcmp(argv[i], "--tempo") && i + 1 < argc) {
            tempo = float(std::atof(argv[++i]));
        } else if (!std::strcmp(argv[i], "--sr") && i + 1 < argc) {
            sr = float(std::atof(argv[++i]));
        }
    }
    std::ifstream in(argv[2], std::ios::binary);
    std::vector<float> x((std::istreambuf_iterator<char>(in)), {});
    // istreambuf gives bytes; reinterpret properly
    in.clear();
    in.seekg(0, std::ios::end);
    const size_t bytes = size_t(in.tellg());
    in.seekg(0);
    x.assign(bytes / 4, 0.f);
    in.read(reinterpret_cast<char*>(x.data()), std::streamsize(bytes));
    auto fx = hs2::createEffect(type);
    if (!fx) {
        std::fprintf(stderr, "effect type %d not ported\n", type);
        return 1;
    }
    if (tempo > 0.f) fx->setTempo(tempo, float(double(tempo) / 60.0 / sr));  // element order: +0xc, +8, +4
    float pitchValue = -10.f;
    if (usePitch) fx->setPitchSource(&pitchValue);  // before setSampleRate, as the element does
    fx->setSampleRate(sr);
    for (size_t i = 0; i < params.size() && i < 12; ++i) fx->setParam(int(i), params[i]);
    const int frames = int(x.size() / 2);
    std::vector<uint8_t> flags;
    for (int s = 0; s < frames; s += 32) {
        const int n = std::min(32, frames - s);
        for (const auto& [f, v] : pitch)
            if (f <= s) pitchValue = v;
        if (gated) {  // part render protocol: silent = nothing upstream made sound (all-zero input block)
            bool silent = true;
            for (int k = 0; k < 2 * n; ++k)
                if (x[2 * s + k] != 0.f) silent = false;
            fx->processGated(x.data() + 2 * s, n, silent);
            flags.push_back(silent ? 1 : 0);
        } else {
            fx->process(x.data() + 2 * s, n);
        }
    }
    if (gated) {
        std::ofstream fo(std::string(argv[3]) + ".flags", std::ios::binary);
        fo.write(reinterpret_cast<const char*>(flags.data()), std::streamsize(flags.size()));
    }
    std::ofstream out(argv[3], std::ios::binary);
    out.write(reinterpret_cast<const char*>(x.data()), std::streamsize(x.size() * 4));
    return 0;
}
