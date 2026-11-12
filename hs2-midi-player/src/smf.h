#pragma once
// Standard MIDI File reader for HS2 MIDI Player: formats 0, 1 and 2 (format 2's tracks are played one after the
// other), PPQ and SMPTE divisions, running status, SysEx, the tempo map and time signatures. Every track is merged
// into one list in time order (events at the same tick keep their track order, then their order in the track).
#include <cstdint>
#include <string>
#include <vector>

namespace smf {

struct Event {
    uint32_t tick = 0;
    double sec = 0;    // at the file's own tempo map
    double qn = 0;     // quarter notes from the start (SMPTE files: at 120 BPM)
    uint8_t m[3] = {};
    uint8_t len = 0;   // 1..3 for channel messages, 0 for SysEx
    int32_t sysex = -1;  // index into Song::sysex (F0 messages, F0 included, F7 appended when missing)
};

struct Tempo {
    uint32_t tick = 0;
    double usPerQn = 500000;
    double sec = 0;  // where it starts
};

struct Sig {
    uint32_t tick = 0;
    int num = 4, den = 4;
};

struct Song {
    int format = 0, tracks = 0;
    int ppq = 480;          // ticks per quarter note (SMPTE: ticks per second / 2, see qn)
    bool smpte = false;
    double ticksPerSec = 0; // SMPTE only
    std::string title;      // the first track's name, if any
    std::vector<Event> events;
    std::vector<std::vector<uint8_t>> sysex;
    std::vector<Tempo> tempos;  // never empty, tempos[0].tick == 0
    std::vector<Sig> sigs;      // never empty, sigs[0].tick == 0
    std::vector<uint32_t> bars; // start tick of every bar up to the end (bars[0] = 0)
    uint32_t endTick = 0;       // the last event (End of Track included)
    double endSec = 0, endQn = 0;
    std::vector<std::string> warnings;

    double secAt(uint32_t tick) const;
    double secAt(double tick) const;
    double tickAtSec(double sec) const;
    double bpmAt(uint32_t tick) const;
    double qnOf(double tick) const { return smpte ? tick / ticksPerSec * 2.0 : tick / double(ppq); }
    double tickOfQn(double qn) const { return smpte ? qn / 2.0 * ticksPerSec : qn * ppq; }
    const Sig& sigAt(uint32_t tick) const;
    // 0-based bar and beat (beat in the signature's beat unit, with a fraction) at a tick.
    void barBeat(double tick, int& bar, double& beat) const;
    uint32_t barTick(int bar) const;  // clamped to the bars there are
};

// Throws std::runtime_error with a short reason ("not a MIDI file", ...).
Song parse(const std::vector<uint8_t>& data);
Song load(const std::string& path);

}  // namespace smf
