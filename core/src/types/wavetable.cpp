// Wavetable element (type 3).
//
// Original code:
//   - Element setter FUN_100221f0, per-type cases (docs/engine/element_setter.c, element idx + 0x160):
//       0x12 Bank / 0x13 Wave -> elem+0xbb8 = bank<<8 | wave (trunc(v*128.9) each), FUN_1001c390(3, ...) loads the
//         entry and stores its handle at elem+0xbbc; the zone function FUN_10021e20 copies *handle to elem+0xb94 at
//         note-on (types 1/3/5 branch: no zone lookup, no rand draws; a missing table still plays for type 3).
//       0x15 Sync (+0x884, v > 0.5), 0x16 Semi (+0x88c = floor(float(v*48+0.5)) - 24), 0x17 Cent (+0x890 =
//         2|v-.5|(v-.5)), +0x894 = exp(float((semi + cent) * 0.057762265)) (frequency ratio of osc 1),
//       0x18 Wavetable Aliasing (+0x888 = (short)trunc(v*v*16383): XOR mask applied to every table sample),
//       0x1c Osc2 Wave (+0x880 = trunc(v*7.9)), 0x1d Osc2 Mix (+0x898 = v + 1e-8).
//   - Voice note-on FUN_10014910, case 3 (@10015d6b): rateLn v[0x87] = elem+0x14 (ln(261.62558/sr)); osc1 starts on
//     wave 32 of mip level 0 with phase = period = 0x800000 and +0x114 = 2^23; osc2 phase v[0x10] =
//     fmod(randpm*RandPhase*0.5 + Phase - VelToPhase*vel, 1) - 0.5 (one rand draw); osc2 types 1/2 run one octave
//     up (rateLn += ln2, +0x114 halved); level v[0x98] *= 2(m - m^2) + 1 (m = Osc2 Mix). No note-on matrix pass.
//   - Render mode 3 = FUN_1004c550 (per sample) with the shared control update FUN_1004ab60.
//
// The shared Voice (voice.cpp) supplies the x87-exact note-on, control update, envelopes and LFO 3; this file adds
// the type-3 case of the note-on and the render loop. Measured against the part oracle: about -120 to -145 dB
// (masked for the oracle's 1e-6 bus artefact) on single notes. Shared engine gaps that still show up with
// wavetable patches (voice stealing FUN_100102b0, mono/unison, patch LFO timing) are not handled here.
//
// Wavetable content ("Wavetables\000 Core Wavetables", Milan bank kind 3, 46 entries): slot 1 of an entry is used raw
// (the original's in-memory copy is byte-identical to the file, checked in the emulator). 66312 bytes: an 8-byte
// header (u16 100, u16 256, u16 0, u16 64), then 8 mip levels k = 0..7, each 64 waves of (256 >> k) + 1 int16 samples
// (the last sample is a guard = sample 0). Level 0 = 64 x 257 samples, ..., level 7 = 64 x 3.
//
// Osc 1 (FUN_1004c550): 32-bit integer phase, period 2^23 (index = (phase >> level) >> 15, 15-bit fraction).
//   Per control block: inc = trunc(exp(osc1Pitch) * 2^23 * ratio * v[0x11]) (v[0x11] = min(exp(pitch + rateLn), 1),
//   the osc 2 increment in cycles per sample), at most 0x400000. Each sample: linear interpolation done on the float
//   bit pattern (0x40400000 + s0*128 + ((s1 - s0)*frac >> 8), minus 3.0), samples XORed with the aliasing mask.
//   On each period wrap the mip level is the smallest k with (inc >> k) < 0x10000, and the wave is
//   trunc(Wave * 64) clamped to 0..63 (Wave = elem+0x5c0 + key/vel/random + Wave mods, the "Wave" destination).
// Osc 2: float phase in -0.5..0.5 advancing by v[0x11]; at each wrap the fractional position p (with a correction
//   for the step) is computed and, with Sync on, osc 1's phase is reset (to trunc((phase - 0.5)/inc * inc32), which
//   is <= 0: the next sample reads 1-2 samples before the wave, an original quirk kept as is).
//   Osc2 Wave 0 saw (-p - phase), 1 square (+-0.3, one octave up), 2 saw*1.5 (one octave up), 3 saw*1.5 + p,
//   4 noise (16-bit LCG x91 on the low half of a float near 64, one step added per block), 5 noise * osc1,
//   6 osc1 * saw * 2 (ring), 7 osc1 * saw * 0.5. Mix: x = x + (osc2 - x) * mix.
// Then the shared 2-pole SVF + one-pole stage, taps and gain ramps exactly as the sample loop.
//
// Precision: mirrors the x87 at 53 bits (register values double, float stores where the original stores, fst keeps
// the unrounded register). See the porting rules in docs/NOTES.md.
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

#include "hs2/bank.h"
#include "hs2/content.h"
#include "hs2/element_types.h"

namespace hs2 {

namespace {

float asFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// fistp with RC = chop: out of range / NaN gives the integer indefinite 0x80000000.
int32_t fistpChop(double d) {
    if (!(d > -2147483649.0 && d < 2147483648.0)) return INT32_MIN;
    return int32_t(d);
}

constexpr uint32_t kEntrySize = 66312;  // 8 + 64 * (257 + 129 + 65 + 33 + 17 + 9 + 5 + 3) * 2

// Wavetable banks, loaded on first use (bank number -> entries' slot-1 bytes).
struct WtBank {
    std::vector<std::vector<uint8_t>> entries;
};

const std::vector<uint8_t>* wavetableEntry(const Content* content, int bank, int wave) {
    // Shared by every engine in the process (plugin instances load on their own threads): look up under the lock,
    // read the file outside it, keep the first bank stored. Entries are never removed, so pointers stay valid.
    static std::mutex mu;
    static std::map<std::pair<std::string, int>, std::unique_ptr<WtBank>> cache;
    if (!content) return nullptr;
    const auto key = std::make_pair(content->root().string(), bank);
    const WtBank* found = nullptr;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) found = it->second.get(), have = true;
    }
    if (!have) {
        std::unique_ptr<WtBank> wb;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(content->root() / "Wavetables", ec)) {
            const auto name = f.path().filename().string();
            if (!f.is_regular_file() || f.path().has_extension() || name.size() < 4 || name[3] != ' ') continue;
            if (!std::isdigit(uint8_t(name[0])) || !std::isdigit(uint8_t(name[1])) || !std::isdigit(uint8_t(name[2])))
                continue;
            if (std::stoi(name.substr(0, 3)) != bank) continue;
            try {
                auto mb = MilanBank::open(f.path());
                if (mb->kind != 3) break;
                wb = std::make_unique<WtBank>();
                for (const auto& e : mb->entries) {
                    auto s = mb->read(e.slot1);
                    wb->entries.emplace_back(s.begin(), s.end());
                }
            } catch (...) {
                wb.reset();
            }
            break;
        }
        std::lock_guard<std::mutex> lk(mu);
        found = cache.emplace(key, std::move(wb)).first->second.get();  // emplace keeps an existing entry
    }
    if (!found || wave < 0 || wave >= int(found->entries.size())) return nullptr;
    const auto& v = found->entries[size_t(wave)];
    return v.size() >= kEntrySize ? &v : nullptr;
}

struct WavetableParams : TypeParams {
    const std::vector<uint8_t>* table = nullptr;  // elem+0xb94 (whole entry, header included)
    int osc2Wave = 0;                             // +0x880
    bool sync = false;                            // +0x884
    uint16_t xorMask = 0;                         // +0x888
    float ratio = 1.f;                            // +0x894
    float mix = 1e-8f;                            // +0x898
};

class WavetableVoice : public Voice {
protected:
    // Note-on case 3 (@10015d6b). No note-on matrix pass for this type (FUN_10049dd0 runs only in the Sample case).
    void startOsc(const VoiceStart&, float, MsvcRand& rng) override {
        wp_ = static_cast<const WavetableParams*>(e_->typeParams.get());
        rateLn_ = c_.lnRef;  // v[0x87] = elem+0x14 (maxInc v[0x86] stays 1)
        if (wp_->table) {
            base_ = wp_->table->data();
            tabOff_ = 0x4048;  // wave 32, level 0
            ph_ = 0x800000;
            period_ = 0x800000;
            fperiod_ = 8388608.f;  // +0x114
        } else {
            base_ = zeroTable_;
            tabOff_ = 0;
            ph_ = 0;
            period_ = 0x100;
            fperiod_ = 0.f;
        }
        inc32_ = 0;
        shift_ = 0;
        {  // osc 2 phase v[0x10] (@10015dd9): fmod(randpm * RandPhase / 2 + Phase - VelToPhase * vel, 1) - 0.5
            const double r = (double(rng.bi()) * double(e_->randStart) * 0.5 + double(e_->start)) -
                             double(e_->velToStart) * double(vel_);
            syncPh_ = float(std::fmod(r, 1.0) - 0.5);
        }
        osc2_ = wp_->osc2Wave;
        amp2_ = 0.f;
        switch (osc2_) {
            case 1:
            case 2:  // one octave up
                amp2_ = osc2_ == 1 ? 0.3f : 1.5f;
                rateLn_ = float(double(rateLn_) + double(0.6931471228599548f));
                fperiod_ = fperiod_ * 0.5f;
                break;
            case 4: noise_ = float(double(rng.uni()) * double(0.1f) + 64.0); break;
            case 5: noise_ = float(double(rng.uni()) * double(0.2f) + 128.0); break;
            case 6: amp2_ = 2.f; break;
            default: amp2_ = 0.5f; break;
        }
        std::memcpy(&noiseBits_, &noise_, 4);
        mix_ = wp_->mix;
        {  // level v[0x98] *= 2(m - m^2) + 1 (@10015eb5)
            const double m = mix_;
            levelMod_ = float(((m - m * m) * 2.0 + 1.0) * double(levelMod_));
        }
    }

    bool oscFinished() const override { return false; }

    void renderOsc(float* out, int frames) override {
        const uint8_t* const base = base_;
        const uint16_t mask16 = wp_->xorMask;
        const int32_t mask = int16_t(mask16);  // movsx
        const bool sync = wp_->sync;
        Svf& f = f_[0];
        for (int n = 0; n < frames; ++n) {
            if (--blockCount_ < 0) {
                controlUpdate();
                const double v = x87exp(double(osc1Pitch_)) * double(fperiod_) * double(wp_->ratio) * double(inc_);
                int32_t i = fistpChop(v);
                inc32_ = i > 0x400000 ? 0x400000 : i;
                noiseBits_ = (noiseBits_ & 0xffff0000u) | uint16_t((noiseBits_ & 0xffffu) + 1u);
            }
            // osc 1: linear interpolation on the float bit pattern
            const int32_t u = ph_ >> shift_;
            const uint8_t* t = base + tabOff_ + ptrdiff_t(u >> 15) * 2;
            const int32_t s0 = int32_t(int16_t(rd16(t))) ^ mask;
            const int32_t s1 = int32_t(int16_t(rd16(t + 2))) ^ mask;
            const int32_t d = int32_t(int64_t(s1 - s0) * int32_t(u & 0x7fff)) >> 8;
            const uint32_t bits = uint32_t(d) + (uint32_t(s0 + 0x808000) << 7);
            double x = double(asFloat(bits)) - 3.0;
            const int32_t inc = inc32_;
            ph_ += inc;
            if (ph_ >= period_) {
                ph_ -= period_;
                shift_ = 0;
                int off = 8, len = 0x101;
                int32_t r = inc;
                while (r & int32_t(0xffff0000)) {
                    ++shift_;
                    off += len << 7;
                    r = inc >> shift_;
                    len = (len >> 1) + 1;
                }
                int w = fistpChop(double(wave_) * 64.0);
                w = w < 0 ? 0 : w > 0x3f ? 0x3f : w;
                tabOff_ = off + w * len * 2;
            }
            // osc 2
            double s = double(syncPh_) + double(inc_);
            float p = 0.f;
            if (s > 0.5) {
                if (sync) ph_ = fistpChop((double(syncPh_) - 0.5) / double(inc_) * double(inc));
                p = float((0.5 - s) / double(inc_) + 1.0);
                if (osc2_ == 1) p = float(double(p) * double(amp2_) * 2.0);
                amp2_ = -amp2_;
                s -= 1.0;
            }
            syncPh_ = float(s);
            double o;
            switch (osc2_) {
                case 0: o = -double(p) - s; break;
                case 1: o = double(p) + double(amp2_); break;
                case 2: o = s * double(amp2_); break;
                case 3: o = (double(amp2_) + 1.0) * s + double(p); break;
                case 4:
                    o = double(asFloat(noiseBits_)) - 64.25;
                    noiseBits_ = (noiseBits_ & 0xffff0000u) | uint16_t((noiseBits_ & 0xffffu) * 0x5bu);
                    break;
                case 5:
                    o = (double(asFloat(noiseBits_)) - 128.5) * x;
                    noiseBits_ = (noiseBits_ & 0xffff0000u) | uint16_t((noiseBits_ & 0xffffu) * 0x5bu);
                    break;
                default: o = s * double(amp2_) * x; break;
            }
            x = (o - x) * double(mix_) + x;
            // shared SVF + one-pole (same as the sample loop, x87 order)
            const double hp = ((double(kk_) * double(f.bp) - x) - double(f.xprev)) + double(f.lp);
            f.hp = float(hp);
            const double bp = double(f.bp) - hp * double(Voice::a_);
            f.bp = float(bp);
            f.lp = float(bp * double(b_) + double(f.lp));
            f.xprev = float(x);
            const double tt = double(f.t) - ((double(f.t) - double(f.lp)) + double(f.u)) * double(cc_);
            f.t = float(tt);
            f.u = float(tt * double(cc_) + double(f.u));
            double y;
            switch (tap_) {
                case 0: y = f.lp; break;
                case 1: y = f.bp; break;
                case 2: y = f.hp; break;
                case 3: y = f.u; break;
                case 4: y = f.t; break;
                default: y = 0.0; break;
            }
            if (invert_) y = double(f.xprev) * 2.0 - y;
            const double gl = double(gainL_) + double(incL_);
            gainL_ = float(gl);
            out[2 * n] = float(gl * y + double(out[2 * n]));
            const double gr = double(gainR_) + double(incR_);
            gainR_ = float(gr);
            out[2 * n + 1] = float(gr * y + double(out[2 * n + 1]));
        }
    }

private:
    const WavetableParams* wp_ = nullptr;
    void typeRefresh() override {  // FUN_10049eb0: FIX: also the osc 2 mix word (+0x11c <- +0x898)
        if (!e_->typeParams) return;
        wp_ = static_cast<const WavetableParams*>(e_->typeParams.get());
        mix_ = wp_->mix;
    }
    const uint8_t* base_ = nullptr;
    static constexpr uint8_t zeroTable_[8] = {};  // FIX: no table: reads v[0xbb] (0) with phase 0, inc 0
    int tabOff_ = 0;
    int32_t ph_ = 0, period_ = 0x800000, inc32_ = 0;
    int shift_ = 0;
    float fperiod_ = 8388608.f;
    float syncPh_ = 0.f, amp2_ = 0.f, mix_ = 0.f, noise_ = 0.f;
    uint32_t noiseBits_ = 0;
    int osc2_ = 0;
};

}  // namespace

bool WavetableSupported() { return true; }

std::shared_ptr<const TypeParams> computeWavetableParams(const ParamArray& p, const ParamArray&, const ParamArray&,
                                                         const EngineConsts& c, ElementParams& e) {
    auto w = std::make_shared<WavetableParams>();
    w->table = wavetableEntry(c.content, e.bank, e.wave);  // 0x12 Bank, 0x13 Wave: trunc(v*128.9)
    w->osc2Wave = int(double(p[0x1c]) * double(7.9f));
    w->sync = p[0x15] > 0.5f;
    {
        const float x = float(double(p[0x16]) * 48.0 + 0.5);
        const double semi = std::floor(double(x)) - 24.0;
        const double d = double(p[0x17]) - 0.5;
        const double cent = std::fabs(d) * d * 2.0;  // Cent is set after Semi: the unrounded register is used
        w->ratio = float(x87exp(double(float((cent + semi) * double(0.0577622652053833f)))));
    }
    w->xorMask = uint16_t(int(double(p[0x18]) * double(p[0x18]) * 16383.0));
    w->mix = float(double(p[0x1d]) + double(9.99999993922529e-09f));
    return w;
}

std::unique_ptr<Voice> makeWavetableVoice() { return std::make_unique<WavetableVoice>(); }

}  // namespace hs2
