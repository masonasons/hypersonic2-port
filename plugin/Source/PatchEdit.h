#pragma once
// The patch editor's model: one part's patch parameters, the parameters of each element of its patch, and the patch's
// own send effects, with the original's names and display texts (param_tables.h, FxText.h). It follows the Schwung
// module's editor (schwung/src/hs2_module.cpp): the same pages, names, texts and steps.
//
// An edit address is (element, index):
//   element kSendFxElem (-1): the part's patch send effects, engine part parameter j = 56 + slot * 24 + j' (index = j)
//   element 0: the patch, patch parameter index 0..255
//   element 1..64: an element of the patch (setter index = element record + 1), parameter index 0..255
//   element kSendFxElem, index kSendTypeIdx + slot: a patch send effect's effect type (setter 0x13 of element
//     0x41 + slot, which is not a part parameter)
//   element 1..64 index kPresetIdx, element kSendFxElem index kPresetIdx + slot: an effect preset row. It is no
//     parameter: choosing a preset makes the edits presetEdits() lists (the type and the twelve knobs).
#include <juce_core/juce_core.h>

#include <functional>
#include <utility>
#include <vector>

namespace hs2 {
class Content;
}

namespace hs2port {

class PatchLibrary;

constexpr int kSendFxElem = -1;
constexpr int kSendFxFirstJ = 56;  // part parameter j of send effect 1's Mute
constexpr int kSendFxSlots = 4, kSendFxParams = 24;
constexpr int kSendTypeIdx = 0x200;  // + slot: a patch send effect's effect type
constexpr int kPresetIdx = 0x300;    // an insert's effect preset row (+ slot for a patch send effect)

// What the texts need to know about the part being edited.
struct EditContext {
    std::function<float(int elem, int idx)> value;  // the current values (of the element being edited and the patch)
    std::function<int(int elem)> elemType;          // element type 0..6 (0 = insert effect), -1 = no such element
    std::function<int(int slot)> sendType;          // the effect type in patch send effect slot 0..3, 128 = none
    const PatchLibrary* library = nullptr;          // sample and wavetable bank names (may be null)
    hs2::Content* content = nullptr;                // sample bank instrument names (may be null)
};

struct EditGroup {
    juce::String title;
    std::vector<int> idx;  // the group's shown parameters (those with a name)
};

// A group longer than this is split into numbered groups ("Analog, 1 of 2"), so every group fits on the page.
constexpr int kMaxGroupRows = 16;

std::vector<EditGroup> elementGroups(const EditContext& c, int elem);
std::vector<EditGroup> patchGroups(const EditContext& c);
EditGroup sendFxGroup(const EditContext& c, int slot);  // "Send effect 1: Hall Reverb"

juce::String editName(const EditContext& c, int elem, int idx);  // empty: not shown
juce::String editText(const EditContext& c, int elem, int idx, float v);
bool editReadOnly(const EditContext& c, int elem, int idx);  // Element Type
// What a row shows: the parameter's value, or for an effect preset row the matching preset (presetEdits' value).
float editValue(const EditContext& c, int elem, int idx);
// Effect preset rows (kPresetIdx): choosing value v makes these edits (index in elem's address space, value): the
// effect type, then the twelve knobs. Empty for "Custom". presetTargets: the indexes a preset row's edits use.
bool editIsPreset(int elem, int idx);
std::vector<std::pair<int, float>> presetEdits(const EditContext& c, int elem, int idx, float v);
std::vector<int> presetTargets(int elem, int idx);
// The values of a setting with a short list of them (value to set, text); empty: a slider.
std::vector<std::pair<float, juce::String>> editChoices(const EditContext& c, int elem, int idx);
// The value `delta` texts away from v (the next value that reads differently).
float editStep(const EditContext& c, int elem, int idx, float v, int delta);

juce::String elementTypeName(int type);  // "Analog", "Sample", ... "Effect"

// Part parameters Keep Settings (j 25) and Level Trim (j 26), which are host parameters.
juce::String keepText(float v);
juce::String levelTrimText(float v);
std::vector<std::pair<float, juce::String>> levelTrimChoices();

}  // namespace hs2port
