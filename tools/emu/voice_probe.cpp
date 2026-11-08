// voice_probe: print the port's sample-voice state per sample, to line up with tools/emu/probe output of the
// emulated original (part_oracle.PartOracle + voice struct reads). Debug only; exposes privates with a #define.
// Build (MinGW): g++ -std=c++20 -O1 -I core/include tools/emu/voice_probe.cpp <build>/core/libhs2core.a -o voice_probe
// Usage: voice_probe <bank> <patch> <note> <vel> <samples> [solo] [every] [skip]
#include <cstdio>
#include <cstdlib>
#include <string>
// standard headers first: the #define must only touch the hs2 headers
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <unordered_map>
#include <vector>

#define private public
#include "hs2/content.h"
#include "hs2/patch.h"
#include "hs2/synth.h"
#undef private

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: voice_probe bank patch note vel samples [solo] [every]\n");
        return 2;
    }
    const int bank = std::atoi(argv[1]), prog = std::atoi(argv[2]), note = std::atoi(argv[3]),
              vel = std::atoi(argv[4]), ns = std::atoi(argv[5]);
    const int solo = argc > 6 ? std::atoi(argv[6]) : -1, every = argc > 7 ? std::atoi(argv[7]) : 32;
    hs2::Content c("C:\\Hypersonic 2 Content");
    auto pb = hs2::MilanBank::open(c.patchBankPath(bank));
    auto patch = hs2::Patch::parse(pb->read(pb->entries[prog].slot1));
    for (int k = 0; k < 4; ++k) patch.params[0x3f + k] = 0.f;
    if (solo >= 0)
        for (int k = 0; k < int(patch.elements.size()); ++k)
            if (k != solo) patch.elements[k].params[0] = 1.f;
    hs2::PatchPlayer pl(c, 44100.f);
    pl.load(patch);
    float buf[64] = {};
    pl.render(buf, 32);
    pl.noteOn(note, vel);
    hs2::Voice* v = nullptr;
    for (auto& x : pl.voices_)
        if (x.active()) { v = &x; break; }
    if (!v) { std::printf("no voice\n"); return 1; }
    const auto& e = *v->e_;
    std::printf("tap %d inv %d resoLimit %g c2 %g freqFlag %d ch %d pos %lld end %lld loop %lld rateLn %.9g reso %.9g "
                "cutoffLn %.9g cutoffKey %.9g fenvAmt %.9g\n",
                v->tap_, v->invert_, v->resoLimit_, v->c2Scale_, v->freqFlag_, v->ch_, (long long)v->pos_,
                (long long)v->end_, (long long)v->loopLen_, v->rateLn_, e.reso, e.cutoffLn, e.cutoffKey, e.fenvAmount);
    const int skip = argc > 8 ? std::atoi(argv[8]) : 0;
    for (int t = 0; t < skip; ++t) {
        float o[2] = {0, 0};
        pl.render(o, 1);
    }
    for (int s = 0; s < ns; ++s) {
        float o[2] = {0, 0};
        pl.render(o, 1);
        if (s % every == 0 || s < 3)
            std::printf("%d %+.7f ph %.5f pos %lld y0 %.6f sl %.6f a %.5f b %.5f c %.5f k %.5f gL %.6f lp %.6f bp %.6f "
                        "t %.6f u %.6f fenv %.9g aenv %.9g cut %.5f lfo1 %.6f amp %.6f inc %.9g\n",
                        s, o[0], v->phase_, (long long)v->pos_, v->y0_[0], v->slope_[0], v->a_, v->b_, v->cc_, v->kk_,
                        v->gainL_, v->f_[0].lp, v->f_[0].bp, v->f_[0].t, v->f_[0].u, v->fenv_.level, v->aenv_.level,
                        v->cutoffBase_, v->lfo1Val_, v->amp_, v->inc_);
    }
    return 0;
}
