#include "smf.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace smf {

namespace {

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool more() const { return p < end; }
    uint8_t u8() {
        if (p >= end) throw std::runtime_error("the file ends early");
        return *p++;
    }
    uint32_t be(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = v << 8 | u8();
        return v;
    }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t b = u8();
            v = v << 7 | (b & 0x7f);
            if (!(b & 0x80)) return v;
        }
        throw std::runtime_error("bad variable-length number");
    }
};

struct Raw {
    uint32_t tick;
    int track, seq;
    Event e;
    bool tempo = false, sig = false;
    double usPerQn = 0;
    int num = 4, den = 4;
};

int channelLen(uint8_t st) {
    const uint8_t t = st & 0xf0;
    return t == 0xc0 || t == 0xd0 ? 2 : 3;
}

}  // namespace

Song parse(const std::vector<uint8_t>& data) {
    Song s;
    Reader r{data.data(), data.data() + data.size()};
    // A RIFF RMID wrapper: the MIDI file is its "data" chunk.
    if (data.size() > 20 && std::equal(data.begin(), data.begin() + 4, "RIFF")) {
        for (size_t o = 12; o + 8 <= data.size();) {
            const uint32_t n = data[o + 4] | data[o + 5] << 8 | data[o + 6] << 16 | uint32_t(data[o + 7]) << 24;
            if (std::equal(data.begin() + long(o), data.begin() + long(o) + 4, "data")) {
                r = Reader{data.data() + o + 8, data.data() + std::min(data.size(), o + 8 + size_t(n))};
                break;
            }
            o += 8 + n + (n & 1);
        }
    }
    if (r.end - r.p < 14 || !std::equal(r.p, r.p + 4, "MThd")) throw std::runtime_error("not a MIDI file");
    r.p += 4;
    const uint32_t hlen = r.be(4);
    if (hlen < 6) throw std::runtime_error("bad MIDI header");
    const uint8_t* hend = r.p + hlen;
    s.format = int(r.be(2));
    const int ntracks = int(r.be(2));
    const uint16_t div = uint16_t(r.be(2));
    r.p = hend;
    if (s.format > 2) throw std::runtime_error("unknown MIDI file format " + std::to_string(s.format));
    if (div & 0x8000) {
        const int fps = -int(int8_t(div >> 8));
        const int tpf = div & 0xff;
        if (fps <= 0 || tpf <= 0) throw std::runtime_error("bad SMPTE timing");
        s.smpte = true;
        s.ticksPerSec = double(fps == 29 ? 29.97 : fps) * tpf;
        s.ppq = std::max(1, int(s.ticksPerSec / 2));
    } else {
        if (div == 0) throw std::runtime_error("bad timing (0 ticks per quarter note)");
        s.ppq = div;
    }

    std::vector<Raw> raw;
    uint32_t trackOffset = 0;  // format 2: tracks follow one another
    int seq = 0;
    for (int t = 0; t < ntracks && r.end - r.p >= 8; ++t) {
        const bool isTrack = std::equal(r.p, r.p + 4, "MTrk");
        r.p += 4;
        const uint32_t len = r.be(4);
        const uint8_t* tend = r.p + std::min<size_t>(len, size_t(r.end - r.p));
        if (!isTrack) {  // an unknown chunk: skipped
            r.p = tend;
            --t;
            continue;
        }
        ++s.tracks;
        Reader tr{r.p, tend};
        uint32_t tick = trackOffset;
        uint8_t running = 0;
        bool ended = false;
        try {
            while (tr.more() && !ended) {
                tick += tr.vlq();
                uint8_t b = tr.u8();
                Raw x{tick, t, seq++, {}};
                x.e.tick = tick;
                if (b == 0xff) {
                    const uint8_t type = tr.u8();
                    const uint32_t n = tr.vlq();
                    if (size_t(tr.end - tr.p) < n) throw std::runtime_error("the file ends early");
                    const uint8_t* d = tr.p;
                    tr.p += n;
                    if (type == 0x2f) {
                        ended = true;
                        s.endTick = std::max(s.endTick, tick);
                    } else if (type == 0x51 && n >= 3) {
                        x.tempo = true;
                        x.usPerQn = double(d[0] << 16 | d[1] << 8 | d[2]);
                        if (x.usPerQn > 0) raw.push_back(x);
                    } else if (type == 0x58 && n >= 2) {
                        x.sig = true;
                        x.num = std::max(1, int(d[0]));
                        x.den = 1 << std::min(6, int(d[1]));
                        raw.push_back(x);
                    } else if (type == 0x03 && s.title.empty() && t == 0) {
                        s.title.assign(reinterpret_cast<const char*>(d), n);
                    }
                    continue;  // meta events cancel nothing (running status stays)
                }
                if (b == 0xf0 || b == 0xf7) {
                    const uint32_t n = tr.vlq();
                    if (size_t(tr.end - tr.p) < n) throw std::runtime_error("the file ends early");
                    std::vector<uint8_t> sx;
                    if (b == 0xf0) sx.push_back(0xf0);
                    sx.insert(sx.end(), tr.p, tr.p + n);
                    tr.p += n;
                    running = 0;
                    if (b == 0xf0 && !sx.empty()) {
                        if (sx.back() != 0xf7) sx.push_back(0xf7);
                        x.e.sysex = int32_t(s.sysex.size());
                        s.sysex.push_back(std::move(sx));
                        raw.push_back(x);
                    }
                    continue;
                }
                if (b & 0x80) {
                    if (b >= 0xf0) continue;  // stray system bytes: no data in a file
                    running = b;
                    x.e.m[0] = b;
                    x.e.m[1] = tr.u8() & 0x7f;
                } else {
                    if (!running) continue;  // data without a status: skipped
                    x.e.m[0] = running;
                    x.e.m[1] = b & 0x7f;
                }
                x.e.len = uint8_t(channelLen(x.e.m[0]));
                if (x.e.len == 3) x.e.m[2] = tr.u8() & 0x7f;
                raw.push_back(x);
                s.endTick = std::max(s.endTick, tick);
            }
        } catch (const std::runtime_error& e) {
            s.warnings.push_back("track " + std::to_string(t + 1) + ": " + e.what());
        }
        if (s.format == 2) trackOffset = std::max(trackOffset, tick);
        r.p = tend;
    }
    if (s.tracks == 0) throw std::runtime_error("no tracks in the file");

    std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track != b.track) return a.track < b.track;
        return a.seq < b.seq;
    });

    s.tempos.push_back({0, 500000, 0});
    s.sigs.push_back({0, 4, 4});
    for (const auto& x : raw) {
        if (x.tempo && !s.smpte) {
            if (x.tick == s.tempos.back().tick) s.tempos.back().usPerQn = x.usPerQn;
            else s.tempos.push_back({x.tick, x.usPerQn, 0});
        } else if (x.sig) {
            if (x.tick == s.sigs.back().tick) s.sigs.back() = {x.tick, x.num, x.den};
            else s.sigs.push_back({x.tick, x.num, x.den});
        }
    }
    for (size_t i = 1; i < s.tempos.size(); ++i)
        s.tempos[i].sec = s.tempos[i - 1].sec +
                          double(s.tempos[i].tick - s.tempos[i - 1].tick) * s.tempos[i - 1].usPerQn / 1e6 / s.ppq;
    for (auto& x : raw) {
        if (x.tempo || x.sig) continue;
        x.e.sec = s.secAt(x.tick);
        x.e.qn = s.qnOf(double(x.tick));
        s.events.push_back(x.e);
    }
    s.endSec = s.secAt(s.endTick);
    s.endQn = s.qnOf(double(s.endTick));
    // Bars: each signature's bar length; a signature change before a bar is full starts the next bar there.
    double t = 0;
    size_t gi = 0;
    while (s.bars.size() < 200000) {
        while (gi + 1 < s.sigs.size() && double(s.sigs[gi + 1].tick) <= t) ++gi;
        s.bars.push_back(uint32_t(t));
        double next = t + std::max(1.0, double(s.ppq) * 4.0 * s.sigs[gi].num / s.sigs[gi].den);
        if (gi + 1 < s.sigs.size() && double(s.sigs[gi + 1].tick) < next) next = double(s.sigs[gi + 1].tick);
        if (next >= double(s.endTick)) break;  // a bar starting at the end is no bar
        t = next;
    }
    return s;
}

Song load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open the file");
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() > (64u << 20)) throw std::runtime_error("the file is too large");
    return parse(d);
}

double Song::secAt(uint32_t tick) const { return secAt(double(tick)); }

double Song::secAt(double tick) const {
    if (smpte) return tick / ticksPerSec;
    size_t i = tempos.size() - 1;
    while (i > 0 && double(tempos[i].tick) > tick) --i;
    return tempos[i].sec + (tick - double(tempos[i].tick)) * tempos[i].usPerQn / 1e6 / ppq;
}

double Song::tickAtSec(double sec) const {
    if (smpte) return sec * ticksPerSec;
    size_t i = tempos.size() - 1;
    while (i > 0 && tempos[i].sec > sec) --i;
    return double(tempos[i].tick) + (sec - tempos[i].sec) * 1e6 / tempos[i].usPerQn * ppq;
}

double Song::bpmAt(uint32_t tick) const {
    if (smpte) return 120.0;
    size_t i = tempos.size() - 1;
    while (i > 0 && tempos[i].tick > tick) --i;
    return 60e6 / tempos[i].usPerQn;
}

const Sig& Song::sigAt(uint32_t tick) const {
    size_t i = sigs.size() - 1;
    while (i > 0 && sigs[i].tick > tick) --i;
    return sigs[i];
}

void Song::barBeat(double tick, int& bar, double& beat) const {
    const auto it = std::upper_bound(bars.begin(), bars.end(), uint32_t(std::max(0.0, tick)));
    bar = std::max(0, int(it - bars.begin()) - 1);
    const Sig& g = sigAt(bars[size_t(bar)]);
    const double beatLen = double(ppq) * 4.0 / g.den;
    beat = (tick - double(bars[size_t(bar)])) / beatLen;
}

uint32_t Song::barTick(int bar) const {
    if (bar <= 0) return 0;
    if (size_t(bar) >= bars.size()) return bars.back();
    return bars[size_t(bar)];
}

}  // namespace smf
