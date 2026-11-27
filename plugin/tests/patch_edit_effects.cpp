// The patch editor's effect types, effect presets and wave names (PatchEdit.h) against real content, without the
// plugin: hs2_patch_edit_effects "<Hypersonic 2 Content>". Uses factory patches 000/86 "Mega Pad" (Analog element,
// insert Quad Chorus, send effects) and 000/33 "Dark Driver Arp" (a wavetable element). Exit code 0: all checks pass.
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

#include "../Source/PatchEdit.h"
#include "../Source/PatchLibrary.h"
#include "hs2/bank.h"
#include "hs2/content.h"
#include "hs2/effects.h"
#include "hs2/patch.h"

namespace hs2port {
// PluginProcessor.cpp's, which this check does not link
juce::String fromLatin1(const std::string& s) {
    juce::String out;
    for (unsigned char ch : s) out += juce::juce_wchar(ch);
    return out;
}
}  // namespace hs2port

using namespace hs2port;

namespace {
int failures = 0;
void check(bool ok, const juce::String& what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (!ok) ++failures;
}

hs2::Patch loadPatch(hs2::Content& content, int bank, int index) {
    auto mb = hs2::MilanBank::open(content.patchBankPath(bank));
    return hs2::Patch::parse(mb->read(mb->entries.at(size_t(index)).slot1));
}

// An editor's view of one patch: its stored values, with edits on top (as the plugin's snapshot and edit map).
struct Model {
    hs2::Patch patch;
    std::map<std::pair<int, int>, float> edits;
    EditContext ctx;
    Model(hs2::Patch p, const PatchLibrary& lib, hs2::Content& content) : patch(std::move(p)) {
        ctx.library = &lib;
        ctx.content = &content;
        ctx.value = [this](int elem, int idx) {
            if (auto it = edits.find({elem, idx}); it != edits.end()) return it->second;
            if (elem == 0) return idx >= 0 && idx < 256 ? patch.params[size_t(idx)] : 0.f;
            if (elem >= 1 && elem <= int(patch.elements.size()) && idx >= 0 && idx < 256)
                return patch.elements[size_t(elem - 1)].params[size_t(idx)];
            return 0.f;
        };
        ctx.elemType = [this](int elem) {
            return elem >= 1 && elem <= int(patch.elements.size())
                       ? int(patch.elements[size_t(elem - 1)].params[1] * 15.9f) : -1;
        };
        ctx.sendType = [this](int slot) {
            if (auto it = edits.find({kSendFxElem, kSendTypeIdx + slot}); it != edits.end())
                return int(it->second * 128.9f);
            return 128;  // the part's own send slots are empty in engine mode (their records go to the global part)
        };
    }
    void apply(const std::vector<std::pair<int, float>>& list, int elem) {
        for (const auto& [idx, v] : list) edits[{elem, idx}] = v;
    }
};

bool hasChoice(const std::vector<std::pair<float, juce::String>>& c, const juce::String& text) {
    for (const auto& e : c)
        if (e.second == text) return true;
    return false;
}

float valueOf(const std::vector<std::pair<float, juce::String>>& c, const juce::String& text) {
    for (const auto& e : c)
        if (e.second == text) return e.first;
    throw std::runtime_error("no choice " + text.toStdString());
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("usage: hs2_patch_edit_effects \"<Hypersonic 2 Content>\"");
        return 2;
    }
    try {
        const std::filesystem::path root(juce::String::fromUTF8(argv[1]).toWideCharPointer());
        const auto lib = PatchLibrary::scan(root);
        hs2::Content content(root);
        int namedTypes = 0;
        for (int t = 0; t < 128; ++t) namedTypes += hs2::effectTypeExists(t);

        // ---- wave names
        Model pad(loadPatch(content, 0, 86), lib, content);
        check(pad.ctx.elemType(1) == 5 && pad.ctx.elemType(2) == 0, "Mega Pad: element 1 Analog, element 2 an insert");
        const auto waves = editChoices(pad.ctx, 1, 0x22);
        check(waves.size() == lib.wavetableWaves[0].size() && waves.size() >= 40,
              "Analog Wavetable lists bank 0's waves by name (" + juce::String(int(waves.size())) + ")");
        if (!waves.empty()) {
            check(waves[0].second.startsWith("1 ") && waves[0].second.length() > 2, "first wave: " + waves[0].second);
            check(editText(pad.ctx, 1, 0x22, waves[3].first) == waves[3].second, "wave 4 reads " + waves[3].second);
            check(editStep(pad.ctx, 1, 0x22, waves[3].first, 1) == waves[4].first, "a step goes to the next wave");
        }
        Model arp(loadPatch(content, 0, 33), lib, content);
        check(arp.ctx.elemType(1) == 3, "Dark Driver Arp: element 1 Wavetable");
        const int wtBank = int(arp.ctx.value(1, 0x12) * 128.9f);
        const auto wtWaves = editChoices(arp.ctx, 1, 0x13);
        check(!wtWaves.empty() && wtWaves.size() == lib.wavetableWaves[size_t(wtBank)].size(),
              "Wavetable element's Wave lists bank " + juce::String(wtBank) + "'s waves (" +
                  juce::String(int(wtWaves.size())) + ")");

        // ---- the insert's effect type and preset
        const auto groups = elementGroups(pad.ctx, 2);
        const bool typeRow = !groups.empty() && std::find(groups[0].idx.begin(), groups[0].idx.end(), 0x13) != groups[0].idx.end();
        const bool presetRow = !groups.empty() &&
                               std::find(groups[0].idx.begin(), groups[0].idx.end(), kPresetIdx) != groups[0].idx.end();
        check(typeRow && presetRow, "the Effect group has Effect Type and Effect Preset");
        check(!editReadOnly(pad.ctx, 2, 0x13), "Effect Type is editable");
        check(editName(pad.ctx, 2, kPresetIdx) == "Effect Preset", "the preset row is named Effect Preset");
        const auto types = editChoices(pad.ctx, 2, 0x13);
        check(int(types.size()) == namedTypes + 1 && types[0].second == "No effect",
              "Effect Type lists No effect and the " + juce::String(namedTypes) + " effect types");
        check(!hasChoice(types, "?") && !hasChoice(types, "(none)"), "only real effect types are listed");
        check(editText(pad.ctx, 2, 0x13, pad.ctx.value(2, 0x13)) == "Quad Chorus", "the insert reads Quad Chorus");
        const float flanger = valueOf(types, "Flanger");
        pad.edits[{2, 0x13}] = flanger;
        check(editText(pad.ctx, 2, 0x13, flanger) == "Flanger" && editName(pad.ctx, 2, 0x19).isNotEmpty(),
              "Flanger chosen: its knobs are named (" + editName(pad.ctx, 2, 0x19) + ")");
        check(editStep(pad.ctx, 2, 0x13, flanger, 1) == types[size_t(std::distance(types.begin(),
                  std::find_if(types.begin(), types.end(), [](auto& e) { return e.second == "Flanger"; })) + 1)].first,
              "a step goes to the next effect type");

        const auto presets = editChoices(pad.ctx, 2, kPresetIdx);
        check(presets.size() == lib.effects.size() + 1 && presets[0].second == "Custom (no preset)",
              "Effect Preset lists Custom and the " + juce::String(int(lib.effects.size())) + " effect presets");
        check(editText(pad.ctx, 2, kPresetIdx, editValue(pad.ctx, 2, kPresetIdx)) == "Custom (no preset)",
              "an edited insert reads Custom");
        if (presets.size() > 5) {
            const auto& pick = presets[5];
            const auto edits = presetEdits(pad.ctx, 2, kPresetIdx, pick.first);
            check(edits.size() == 13 && edits[0].first == 0x13 && edits[1].first == 0x19,
                  "choosing " + pick.second + " sets the type and twelve knobs");
            pad.apply(edits, 2);
            check(editText(pad.ctx, 2, kPresetIdx, editValue(pad.ctx, 2, kPresetIdx)) == pick.second,
                  "the preset row then reads " + pick.second);
            check(editText(pad.ctx, 2, 0x13, pad.ctx.value(2, 0x13)) ==
                      juce::String(hs2::effectName(int(lib.effects[4].type * 128.9f))),
                  "and Effect Type reads the preset's type");
            pad.edits[{2, 0x1a}] = pad.ctx.value(2, 0x1a) < 0.5f ? 0.9f : 0.1f;
            check(editText(pad.ctx, 2, kPresetIdx, editValue(pad.ctx, 2, kPresetIdx)) == "Custom (no preset)",
                  "a knob changed afterwards: Custom again");
            check(presetEdits(pad.ctx, 2, kPresetIdx, presets[0].first).empty(), "choosing Custom changes nothing");
            check(presetTargets(2, kPresetIdx).size() == 13, "Delete on the preset row resets the type and knobs");
        }

        // ---- a patch send effect's type and preset
        const auto send = sendFxGroup(pad.ctx, 0);
        check(send.idx.size() >= 2 && send.idx[0] == kSendTypeIdx && send.idx[1] == kPresetIdx,
              "Send effect 1 starts with Effect Type and Effect Preset");
        check(editName(pad.ctx, kSendFxElem, kSendTypeIdx) == "Effect Type" &&
                  editName(pad.ctx, kSendFxElem, kPresetIdx) == "Effect Preset",
              "the send rows are named");
        check(editText(pad.ctx, kSendFxElem, kSendTypeIdx, 1.f) == "No effect", "an empty send slot reads No effect");
        pad.edits[{kSendFxElem, kSendTypeIdx}] = valueOf(editChoices(pad.ctx, kSendFxElem, kSendTypeIdx), "Hall Reverb");
        check(sendFxGroup(pad.ctx, 0).title == "Send effect 1: Hall Reverb", "the send group then reads Hall Reverb");
        const auto sendPresets = editChoices(pad.ctx, kSendFxElem, kPresetIdx);
        if (sendPresets.size() > 1) {
            const auto edits = presetEdits(pad.ctx, kSendFxElem, kPresetIdx, sendPresets[1].first);
            check(edits.size() == 13 && edits[0].first == kSendTypeIdx && edits[1].first == kSendFxFirstJ + 1,
                  "a send preset sets the send's type and its knobs j 1..12");
            pad.apply(edits, kSendFxElem);
            check(editText(pad.ctx, kSendFxElem, kPresetIdx, editValue(pad.ctx, kSendFxElem, kPresetIdx)) ==
                      sendPresets[1].second,
                  "the send's preset row reads " + sendPresets[1].second);
        }
        const auto slot2 = presetEdits(pad.ctx, kSendFxElem, kPresetIdx + 1, sendPresets.size() > 1 ? sendPresets[1].first : 1.f);
        check(slot2.empty() || (slot2[0].first == kSendTypeIdx + 1 && slot2[1].first == kSendFxFirstJ + kSendFxParams + 1),
              "send effect 2's preset row edits slot 2");
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf(failures == 0 ? "PASS: effect types, effect presets and wave names\n" : "%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
