// Drawbar element (type 1): tonewheel organ from a "Drawbars" content bank (kind 5, 'D').
//
// Original:
//   element setter FUN_100221f0 cases 0x72..0x7c (docs/engine/element_setter.c, "Drawbar.*"), weights copied to the voice
//   by FUN_1004a030, voice note-on FUN_10014910 case 1 (@100157e5), render mode 1 = FUN_10051470, note-off click in
//   FUN_10017880 (@10017c5f), zone function FUN_10021e20 (type 1: the element's loaded entry, no rand draws).
//
// Content format (D bank entry, slot 1): a "SyncrosoftContentHeader" licence blob (8 + u32@27 bytes; the Syncrosoft
// reader FUN_10083a50 returns what follows), then the table, addressed as int16 from its start:
//   +4 float: tune (ln frequency offset), +0x10 int32[12] offsets, +0x40 int32[12] lengths (int16 units), one per
//   semitone (C..B). Each table is frames of 8 int16 = the same tone at octaves 0..7 (column c = 2^c x the column-0
//   frequency), an integer number of cycles long, followed by one guard frame (the first frame again).
//   10 entries in "000 Core Drawbars": Real, Dirty, Clean (3 tables of 21688.. frames x8), Sine, Soft, Saw, Square,
//   Buzz, Bright, Rich.
// Voice: three read pointers into the key's tables (16.16 phase, one shared fraction; step = frames, i.e. 8 int16):
//   pos1 = table[key % 12] + octave       columns 0..4 -> 16', 8', 4', 2', 1'
//   pos2 = table[(key+7) % 12] + octave+1 columns 0..2 -> 5 1/3, 2 2/3, 1 1/3   (a fifth up)
//   pos3 = table[(key+16) % 12] + oct+2   column 0     -> 1 3/5               (a major third, two octaves up)
// Each reads linearly interpolated int16 (integer maths), weighted by integer drawbar levels n^2*32 + 200 (n = 0..8),
// summed as int32, times 7.5e-9. High keys fold the top bars down (FUN_1004a030). Percussion adds trunc(level) to
// the 4' (2nd harmonic) or 2 2/3 (3rd) weight, decaying per 32-sample block. Key click: a 16-bit LCG noise, one-pole
// lowpassed, modulates the amplitude for the first `Click Length` blocks (again at note-off with Off Click).
// Then the shared SVF / gain ramps, as the sample voice, mirrored here with the x87 (53-bit) register values.
//
// The phase increment is trunc(65536 * exp(pitch)) per block, so the pitch path must be exact; the shared Voice
// (voice.cpp) mirrors the x87 for the note-on, control update, envelopes and LFO 3. Original quirk kept here:
// percussion "single trigger" (Perc Trig 0/1/2) reads the level of the part's other sounding drawbar voices.
// Verified against the original (part oracle + the Drawbars bank, Syncrosoft header stripped) on all 66 factory
// drawbar elements (bank 83), --solo, held chords, velocities 5..127, keys 0..127: 58/66 at -113..-143 dB once the
// oracle's 1e-6 per-call bias is excluded (-94..-109 dB with it). The other 8 use shared-engine features not ported
// yet: mono mode (9, 19, 27, 35), arpeggiator / step-sequencer sources (17, 20, 44), unison (37), and patch LFOs that
// free-run while the part is silent (29). Released-voice stealing at note-on (FUN_100102b0) is not ported either.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "hs2/bank.h"
#include "hs2/content.h"
#include "hs2/element_types.h"

namespace hs2 {

namespace {

float bitsF(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
uint32_t fBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct DrawbarTable {
    std::vector<int16_t> d;  // the whole table, int16 units from its start (+ zero padding)
    float tune = 0;
    int off[12] = {}, len[12] = {};
};

// Loaded tables, cached for the process (content files are read-only).
const DrawbarTable* loadTable(Content* content, int bank, int entry) {
    static std::mutex mu;
    static std::map<std::tuple<std::string, int, int>, std::unique_ptr<DrawbarTable>> cache;
    if (!content) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    const auto key = std::make_tuple(content->root().string(), bank, entry);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<DrawbarTable> t;
    try {
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(content->root() / "Drawbars", ec)) {
            const auto name = f.path().filename().string();
            if (!f.is_regular_file() || f.path().has_extension() || name.size() < 4 || name[3] != ' ' ||
                !std::isdigit(uint8_t(name[0])) || !std::isdigit(uint8_t(name[1])) || !std::isdigit(uint8_t(name[2])) ||
                std::stoi(name.substr(0, 3)) != bank)
                continue;
            auto mb = MilanBank::open(f.path());
            if (entry < 0 || entry >= int(mb->entries.size())) break;
            auto s = mb->read(mb->entries[size_t(entry)].slot1);
            // FUN_10019fb0 -> FUN_10083a50: a Syncrosoft content header is replaced by its payload
            while (s.size() > 31 && std::memcmp(s.data(), "SyncrosoftContentHeader", 23) == 0) {
                const size_t skip = 8 + size_t(rd32(s.data() + 27));
                if (skip >= s.size()) {
                    s = {};
                    break;
                }
                s = s.subspan(skip);
            }
            if (s.size() < 0x70) break;
            t = std::make_unique<DrawbarTable>();
            t->tune = rdf(s.data() + 4);
            for (int k = 0; k < 12; ++k) {
                t->off[k] = int(rd32(s.data() + 0x10 + 4 * k));
                t->len[k] = int(rd32(s.data() + 0x40 + 4 * k));
            }
            t->d.assign(s.size() / 2 + 64, 0);
            for (size_t i = 0; i < s.size() / 2; ++i) t->d[i] = int16_t(rd16(s.data() + 2 * i));
            bool ok = true;
            for (int k = 0; k < 12; ++k)
                ok = ok && t->off[k] >= 0 && t->len[k] > 0 && size_t(t->off[k]) + size_t(t->len[k]) + 24 <= t->d.size();
            if (!ok) t.reset();
            break;
        }
    } catch (const std::exception&) {
        t.reset();
    }
    return (cache[key] = std::move(t)).get();
}

struct DrawbarParams : TypeParams {
    const DrawbarTable* table = nullptr;
    int bars[10] = {};          // +0x924..+0x948: 16', 8', 4', 2', 1', 5 1/3, 2 2/3, 1 1/3, 1 3/5, (unused)
    int percTrig = 0;           // +0x94c trunc(v*3.9)
    bool percussion = false;    // +0x950
    int clickLen = 0;           // +0x954 (blocks)
    float percLevel = 4000.f;   // +0x958: 12000 or 4000
    float percDecay = 1.f;      // +0x95c per-block factor
    float percHarm = 0.f;       // +0x960: 1 = third harmonic, 0 = second
    float keyClick = 0.f;       // +0x964 v^2 * 6
    float offClick = 0.f;       // +0x968
};

class DrawbarVoice : public Voice {
public:
    bool percLevel(float& out) const override {  // render mode 1: the percussion level word (+0x164)
        out = perc_;
        return true;
    }

    // Note-off (FUN_10017880): the shared note-off plus the key-up click (@10017c5f).
    void release() override {
        if (!active_ || released_) return;
        const bool click = !waitingForNoteOff_ && delay_ == 0;
        Voice::release();
        if (click && dp_) {
            clickCount_ = dp_->clickLen;
            clickCoef_ = clickCoef_ * 0.5f;
            clickDepth_ = float(double(dp_->offClick) * double(dp_->keyClick));
        }
    }

protected:
    void startOsc(const VoiceStart& vs, float, MsvcRand& rng) override {
        // Note-on case 1 (@100157e5). No note-on matrix pass for this type (FUN_10049dd0 runs only for Sample).
        dp_ = dynamic_cast<const DrawbarParams*>(e_->typeParams.get());
        if (!dp_ || !dp_->table) {  // FUN_10021e20 returns 0 for type 1 without a loaded entry: no voice
            active_ = false;
            return;
        }
        const DrawbarTable& T = *dp_->table;
        tab_ = T.d.data();
        maxInc_ = 4.f;
        phase16_ = 0;
        // key = trunc((note + transpose - 60) * keytrack + semis + octave + 60.5), clamped 36..120
        int key = int(double(vs.note + vs.transpose - 60) * double(e_->keytrack) + double(e_->semis) +
                      double(e_->octaveSemis) + 60.5);
        key = std::clamp(key, 36, 120);
        {
            const double l = std::log(double(1.f / c_.sr) * 44100.0);  // fyl2x with fldln2
            rateLn_ = float(double(60 - key) * double(0.057762265f) + l + double(T.tune));
        }
        int k = key % 12, oct = key / 12 - 3;
        flags_[0] = flags_[1] = flags_[2] = flags_[3] = 0;
        pos1_ = T.off[k] + oct;
        len1_ = T.len[k];
        end1_ = pos1_ - 1 + len1_;
        pos1_ += (rng.next() % len1_) & 0x7ffffff8;
        flags_[0] = key > 78;
        if (key > 90) flags_[1] = key > 102 ? 2 + (key > 114) : 1;
        k += 7;
        if (k > 11) {
            k -= 12;
            ++oct;
        }
        pos2_ = T.off[k] + oct + 1;
        flags_[2] = key > 83;
        if (key > 95) {
            flags_[3] = 1;
            pos2_ -= 1;
            if (key > 107) pos2_ -= (key > 119) + 1;
        }
        len2_ = T.len[k];
        end2_ = len2_ - 1 + pos2_;
        pos2_ += (rng.next() % len2_) & 0x7ffffff8;
        k += 9;
        if (k > 11) {
            k -= 12;
            ++oct;
        }
        oct = std::min(oct, 5);
        pos3_ = T.off[k] + oct + 2;
        len3_ = T.len[k];
        end3_ = len3_ - 1 + pos3_;

        // FIX: percussion level of the other HELD drawbar voices in the part's voice list (max, -1 if none;
        // @10015a9a, PatchPlayer fills VoiceStart::drawbarOther)
        const float other = vs.drawbarOther;
        gain_ = bitsF(0x3200d959u);
        percDecay_ = dp_->percDecay;
        percHarm_ = dp_->percHarm;
        perc_ = 0.f;
        if (dp_->percussion) {
            switch (dp_->percTrig) {
                case 0: perc_ = other <= -0.5f ? dp_->percLevel : other; break;
                case 1: perc_ = (vs.velocity > 0x50 || other <= -0.5f) ? dp_->percLevel : other; break;
                case 2: perc_ = other <= -0.5f ? dp_->percLevel : 0.f; break;
                default: perc_ = dp_->percLevel; break;
            }
            if (dp_->percLevel > 9000.f) gain_ = gain_ * 0.5f;
        }
        percA_ = percB_ = 0;
        noise_ = fBits(float(double(rng.uni()) * double(0.2f) + 128.0));
        clickCount_ = dp_->clickLen;
        if (vs.velocity < 10) clickCount_ = (clickCount_ >> 1) + clickCount_;
        clickS_ = 0.f;
        clickCoef_ = float((double(key - 64) * 200.0 + 20000.0) * double(1.f / c_.sr));
        clickDepth_ = dp_->keyClick;
        copyWeights(*dp_);
    }

    // FUN_1004a030: weights + fold-back of the bars above the table's top octave. Note-on, and (FIX) the render
    // entry whenever the element was set again (the weights are live: patch type 3 maps CC 12..20 to the bars).
    void renderEntry() override {
        if (auto* p = dynamic_cast<const DrawbarParams*>(e_->typeParams.get())) copyWeights(*p);
    }
    void copyWeights(const DrawbarParams& dp) {
        std::copy(dp.bars, dp.bars + 10, w_);
        if (flags_[0]) {
            w_[3] += w_[4] >> 1;
            w_[4] = 0;
        }
        if (flags_[1]) {
            w_[2] += w_[3] >> 1;
            w_[3] = 0;
            if (flags_[1] > 1) {
                w_[1] += w_[2] >> 1;
                w_[2] = 0;
            }
            if (flags_[1] == 3) {
                w_[0] += w_[1] >> 1;
                w_[1] = 0;
            }
        }
        if (flags_[2]) {
            w_[6] += w_[7] >> 1;
            w_[7] = 0;
        }
        if (flags_[3]) {
            const int a = w_[5];
            w_[5] = 0;
            w_[6] = (w_[6] >> 1) + a;
        }
    }

    bool oscFinished() const override { return false; }

    void renderOsc(float* out, int frames) override {
        if (perc_ < 1e-8f) perc_ = 0.f;
        const int16_t* d = tab_;
        Svf& s = f_[0];
        for (int n = 0; n < frames && active_; ++n) {
            if (--blockCount_ < 0) {
                controlUpdate();
                incInt_ = int(double(bitsF(0x47800000u)) * double(inc_));  // 65536 * inc, truncated
                if (clickCount_ != 0) {
                    noise_ = (noise_ & 0xffff0000u) | ((noise_ + 1u) & 0xffffu);
                    if (--clickCount_ == 0) clickDepth_ = 0.f;
                }
                const double p = double(percDecay_) * double(perc_);
                perc_ = float(p);
                percB_ = int(p * double(percHarm_));
                percA_ = int(perc_) - percB_;
            }
            phase16_ += incInt_;
            if (phase16_ > 0xffff) {
                const int adv = (phase16_ >> 13) & ~7;
                pos1_ += adv;
                if (pos1_ > end1_) pos1_ -= len1_;
                pos2_ += adv;
                if (pos2_ > end2_) pos2_ -= len2_;
                pos3_ += adv;
                if (pos3_ > end3_) pos3_ -= len3_;
                phase16_ &= 0xffff;
            }
            const uint32_t fr = uint32_t(phase16_);
            auto lerp = [&](int p) {
                const int a = d[p], b = d[p + 8];
                return (int32_t(uint32_t(b - a) * fr) >> 16) + a;
            };
            uint32_t sum = uint32_t(lerp(pos1_)) * uint32_t(w_[0]);
            sum += uint32_t(lerp(pos1_ + 1)) * uint32_t(w_[1]);
            sum += uint32_t(lerp(pos1_ + 2)) * uint32_t(w_[2] + percA_);
            sum += uint32_t(lerp(pos1_ + 3)) * uint32_t(w_[3]);
            sum += uint32_t(lerp(pos1_ + 4)) * uint32_t(w_[4]);
            sum += uint32_t(lerp(pos2_)) * uint32_t(w_[5]);
            sum += uint32_t(lerp(pos2_ + 1)) * uint32_t(w_[6] + percB_);
            sum += uint32_t(lerp(pos2_ + 2)) * uint32_t(w_[7]);
            sum += uint32_t(lerp(pos3_)) * uint32_t(w_[8]);
            double x = double(int32_t(sum)) * double(gain_);
            if (clickCount_ != 0) {
                const double c = ((double(bitsF(noise_)) - 128.0) - double(clickS_)) * double(clickCoef_) +
                                 double(clickS_);
                clickS_ = float(c);
                x = x - c * double(clickDepth_) * x;
                noise_ = (noise_ & 0xffff0000u) | ((noise_ * 0x5bu) & 0xffffu);
            }
            // shared SVF + one-pole (as FUN_1004b490), x87 register values kept as double
            const double hp = ((double(s.bp) * double(kk_) - x) - double(s.xprev)) + double(s.lp);
            s.hp = float(hp);
            const double bp = double(s.bp) - hp * double(a_);
            s.bp = float(bp);
            s.lp = float(bp * double(b_) + double(s.lp));
            s.xprev = float(x);
            const double t = double(s.t) - ((double(s.t) - double(s.lp)) + double(s.u)) * double(cc_);
            s.t = float(t);
            s.u = float(t * double(cc_) + double(s.u));
            double o;
            switch (tap_) {
                case 0: o = s.lp; break;
                case 1: o = s.bp; break;
                case 2: o = s.hp; break;
                case 3: o = s.u; break;
                case 4: o = s.t; break;
                default: o = 0.0; break;
            }
            if (invert_) o = double(s.xprev) + double(s.xprev) - o;
            const double gl = double(incL_) + double(gainL_);
            gainL_ = float(gl);
            out[2 * n] = float(gl * o + double(out[2 * n]));
            const double gr = double(incR_) + double(gainR_);
            gainR_ = float(gr);
            out[2 * n + 1] = float(gr * o + double(out[2 * n + 1]));
        }
    }

private:
    const DrawbarParams* dp_ = nullptr;
    void typeRefresh() override { if (e_->typeParams) dp_ = static_cast<const DrawbarParams*>(e_->typeParams.get()); }
    const int16_t* tab_ = nullptr;
    int pos1_ = 0, end1_ = 0, len1_ = 1, pos2_ = 0, end2_ = 0, len2_ = 1, pos3_ = 0, end3_ = 0, len3_ = 1;
    int phase16_ = 0, incInt_ = 0;
    int flags_[4] = {};  // voice +0x560..+0x56c
    int w_[10] = {};      // voice +0x538..
    float gain_ = 0;      // +0x1d8
    float perc_ = 0, percDecay_ = 1, percHarm_ = 0;  // +0x164 +0x168 +0x16c
    int percA_ = 0, percB_ = 0;                       // +0x570 (4' add), +0x574 (2 2/3 add)
    uint32_t noise_ = 0;                              // float at +0x120; its low 16 bits are the noise LCG
    int clickCount_ = 0;                              // +0x134 (blocks)
    float clickS_ = 0, clickCoef_ = 0, clickDepth_ = 0;  // +0x158 +0x15c +0x160
};

}  // namespace

bool DrawbarSupported() { return true; }

std::shared_ptr<const TypeParams> computeDrawbarParams(const ParamArray& p, const ParamArray&, const ParamArray&,
                                                     const EngineConsts& c, ElementParams& e) {
    auto dp = std::make_shared<DrawbarParams>();
    const int bank = int(double(p[0x12]) * double(128.9f)), entry = int(double(p[0x13]) * double(128.9f));
    dp->table = loadTable(c.content, bank, entry);
    e.hasData = dp->table != nullptr;  // FUN_10021e20 returns 0 for type 1 without its entry: no voice
    // 0x19..0x21: 16', 5 1/3, 8', 4', 2 2/3, 2', 1 3/5, 1 1/3, 1' -> stored in the order of +0x924..
    static constexpr int kSlot[9] = {0, 5, 1, 2, 6, 3, 8, 7, 4};
    for (int i = 0; i < 9; ++i) {
        const int n = int(double(p[0x19 + i]) * double(8.9f));
        dp->bars[kSlot[i]] = n * n * 32 + (n != 0 ? 200 : 0);
    }
    dp->percTrig = int(double(p[0x14]) * double(3.9f));
    dp->percussion = p[0x15] > 0.5f;
    dp->percLevel = p[0x16] > 0.5f ? 12000.f : 4000.f;
    {
        const float k = p[0x17] > 0.5f ? 6.16f : 5.16f;
        const float a = float(-(x87exp(double(k)) * double(1.f / c.sr)));
        dp->percDecay = float(x87exp(double(a)));
    }
    dp->percHarm = p[0x18] > 0.5f ? 1.f : 0.f;
    dp->keyClick = float(double(p[0x22]) * double(p[0x22]) * 6.0);
    dp->clickLen = int((double(p[0x23]) * double(0.00013f) + double(3e-05f)) * double(c.sr));
    dp->offClick = p[0x24];
    return dp;
}

std::unique_ptr<Voice> makeDrawbarVoice() { return std::make_unique<DrawbarVoice>(); }

}  // namespace hs2
