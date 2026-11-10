#pragma once
// The host parameters beyond the mixer: each part's settings, microtuning and arpeggiator, Link, and the four global
// send effects, as the original Hypersonic 2 exposed them (VST parameters; layout in docs/NOTES.md "VST parameters").
// Names are this port's own; value texts come from the original's display code (ParamText.h).
#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

#include "ParamText.h"

namespace hs2port {

enum class ExtPage { settings, tuning, arp };
enum class ExtKind { table, arpRate, tuneBank, tuneScale };

// One per-part setting: its VST parameter j (engine index part * 0x98 + j).
struct ExtDef {
    int j;
    const char* id;
    const char* name;
    ExtPage page;
    ExtKind kind;
    const paramtext::Table* table;
    int tuning;  // tuneBank / tuneScale: which of the two tunings (0, 1)
};

inline constexpr ExtDef kExtDefs[] = {
    {4, "output", "Output", ExtPage::settings, ExtKind::table, &paramtext::kOutput, 0},
    {15, "keylow", "Key Low", ExtPage::settings, ExtKind::table, &paramtext::kKeyLow, 0},
    {16, "keyhigh", "Key High", ExtPage::settings, ExtKind::table, &paramtext::kKeyHigh, 0},
    {17, "keyfade", "Key Crossfade", ExtPage::settings, ExtKind::table, &paramtext::kKeyXfade, 0},
    {18, "vellow", "Velocity Low", ExtPage::settings, ExtKind::table, &paramtext::kVelLow, 0},
    {19, "velhigh", "Velocity High", ExtPage::settings, ExtKind::table, &paramtext::kVelHigh, 0},
    {20, "velfade", "Velocity Crossfade", ExtPage::settings, ExtKind::table, &paramtext::kVelXfade, 0},
    {21, "transpose", "Transpose", ExtPage::settings, ExtKind::table, &paramtext::kTranspose, 0},
    {22, "fine", "Fine Tune", ExtPage::settings, ExtKind::table, &paramtext::kFineTune, 0},
    {23, "voices", "Max Voices", ExtPage::settings, ExtKind::table, &paramtext::kMaxVoices, 0},
    {24, "fxsends", "Effect Sends", ExtPage::settings, ExtKind::table, &paramtext::kFxSends, 0},
    {27, "pbup", "Pitch Bend Up", ExtPage::settings, ExtKind::table, &paramtext::kPbUp, 0},
    {28, "pbdown", "Pitch Bend Down", ExtPage::settings, ExtKind::table, &paramtext::kPbDown, 0},
    {29, "mono", "Mono / Poly", ExtPage::settings, ExtKind::table, &paramtext::kMonoPoly, 0},
    {30, "glide", "Glide", ExtPage::settings, ExtKind::table, &paramtext::kGlide, 0},
    {31, "glidetime", "Glide Time", ExtPage::settings, ExtKind::table, &paramtext::kGlideTime, 0},
    {32, "tune1bank", "Tuning 1 Bank", ExtPage::tuning, ExtKind::tuneBank, nullptr, 0},
    {33, "tune1scale", "Tuning 1 Scale", ExtPage::tuning, ExtKind::tuneScale, nullptr, 0},
    {34, "tune1root", "Tuning 1 Root", ExtPage::tuning, ExtKind::table, &paramtext::kTuneRoot, 0},
    {35, "tune2bank", "Tuning 2 Bank", ExtPage::tuning, ExtKind::tuneBank, nullptr, 1},
    {36, "tune2scale", "Tuning 2 Scale", ExtPage::tuning, ExtKind::tuneScale, nullptr, 1},
    {37, "tune2root", "Tuning 2 Root", ExtPage::tuning, ExtKind::table, &paramtext::kTuneRoot, 1},
    {38, "tunedepth", "Tuning Depth", ExtPage::tuning, ExtKind::table, &paramtext::kTuneDepth, 0},
    {39, "arp", "Arpeggiator", ExtPage::arp, ExtKind::table, &paramtext::kArpOn, 0},
    {40, "arplatch", "Arp Latch", ExtPage::arp, ExtKind::table, &paramtext::kArpLatch, 0},
    {41, "arpmode", "Arp Mode", ExtPage::arp, ExtKind::table, &paramtext::kArpMode, 0},
    {42, "arpoctaves", "Arp Octaves", ExtPage::arp, ExtKind::table, &paramtext::kArpOctaves, 0},
    {43, "arpsteps", "Arp Steps", ExtPage::arp, ExtKind::table, &paramtext::kArpSteps, 0},
    {44, "arpsync", "Arp Sync", ExtPage::arp, ExtKind::table, &paramtext::kArpSync, 0},
    {45, "arprate", "Arp Rate", ExtPage::arp, ExtKind::arpRate, &paramtext::kArpRate, 0},
    {46, "arpgate", "Arp Gate", ExtPage::arp, ExtKind::table, &paramtext::kArpGate, 0},
    {47, "arpswing", "Arp Swing", ExtPage::arp, ExtKind::table, &paramtext::kArpSwing, 0},
};
inline constexpr int kExtPerPart = int(sizeof(kExtDefs) / sizeof(kExtDefs[0]));  // 32

// One global send effect parameter j (engine index 0x980 + k * 0x18 + j).
enum class GfxKind { table, knob };
struct GfxDef {
    const char* id;
    const char* name;
    GfxKind kind;
    const paramtext::Table* table;
};
inline const GfxDef& gfxDef(int j) {
    static const std::vector<GfxDef> defs = [] {
        std::vector<GfxDef> d;
        d.push_back({"mute", "Mute", GfxKind::table, &paramtext::kFxMute});
        static const char* const knobIds[16] = {"knob1", "knob2", "knob3", "knob4", "knob5", "knob6", "knob7", "knob8",
                                                "knob9", "knob10", "knob11", "knob12", "knob13", "knob14", "knob15",
                                                "knob16"};
        static const char* const knobNames[16] = {"Knob 1", "Knob 2", "Knob 3", "Knob 4", "Knob 5", "Knob 6",
                                                  "Knob 7", "Knob 8", "Knob 9", "Knob 10", "Knob 11", "Knob 12",
                                                  "Knob 13", "Knob 14", "Knob 15", "Knob 16"};
        for (int k = 0; k < 16; ++k) d.push_back({knobIds[k], knobNames[k], GfxKind::knob, nullptr});
        d.push_back({"aux", "Aux Output", GfxKind::table, &paramtext::kFxAux});
        d.push_back({"level", "Level", GfxKind::table, &paramtext::kFxLevel});
        d.push_back({"pan", "Pan", GfxKind::table, &paramtext::kFxPan});
        d.push_back({"send1", "Send 1", GfxKind::table, &paramtext::kFxSend});
        d.push_back({"send2", "Send 2", GfxKind::table, &paramtext::kFxSend});
        d.push_back({"send3", "Send 3", GfxKind::table, &paramtext::kFxSend});
        d.push_back({"send4", "Send 4", GfxKind::table, &paramtext::kFxSend});
        return d;
    }();
    return defs[size_t(j)];
}
inline constexpr int kGfxSlots = 4, kGfxParams = 24;

// ---- value texts and choice lists (shared by the host parameters and the editor)

// The text of a table at value v (0..1): an enum's segment, or the sample nearest v.
inline juce::String tableText(const paramtext::Table& t, float v) {
    v = juce::jlimit(0.f, 1.f, v);
    if (t.kind == paramtext::kEnum) {
        int i = 0;
        while (i + 1 < t.numSegs && t.segs[i + 1].from <= v) ++i;
        if (t.numSegs == 0) return {};
        const auto s = juce::String(t.segs[i].text).trim();
        return s == "<C>" ? juce::String("centre") : s;  // pan centre: read aloud as "centre", not "less than C"
    }
    const int idx = juce::jlimit(0, 1024, juce::roundToInt(v * 1024.f));
    if (t.kind == paramtext::kText) return juce::String(t.texts[idx]).trim();
    const float x = t.nums[idx];
    auto s = juce::String(x, 2);
    while (s.containsChar('.') && (s.endsWithChar('0') || s.endsWithChar('.'))) {
        const bool dot = s.endsWithChar('.');
        s = s.dropLastCharacters(1);
        if (dot) break;
    }
    if (s == "-0") s = "0";
    const juce::String unit(t.unit);
    return unit.isEmpty() ? s : (unit == "%" ? s + " %" : s + " " + unit);
}

// An enum table's choices: (value to set, text). The value is the middle of the choice's range, inside [lo, hi].
inline std::vector<std::pair<float, juce::String>> tableChoices(const paramtext::Table& t, float lo = 0.f, float hi = 1.f) {
    std::vector<std::pair<float, juce::String>> out;
    if (t.kind != paramtext::kEnum) return out;
    for (int i = 0; i < t.numSegs; ++i) {
        const float a = t.segs[i].from, b = i + 1 < t.numSegs ? t.segs[i + 1].from : 1.f;
        const float ca = juce::jmax(a, lo), cb = juce::jmin(b, hi);
        if (cb <= ca) continue;  // outside [lo, hi]
        const auto s = juce::String(t.segs[i].text).trim();
        out.push_back({(ca + cb) * 0.5f, s == "<C>" ? juce::String("centre") : s});
    }
    return out;
}

// Arp Rate: the VST value v is set as patch value v * 0.8 + 0.2 (FUN_1008a950 case 0x2d).
inline float arpRatePatchValue(float v) { return float(double(v) * double(0.8f) + double(0.2f)); }

}  // namespace hs2port
