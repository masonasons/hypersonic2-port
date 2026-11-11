#pragma once
// The patch editor's model: one part's patch parameters, the parameters of each element of its patch, and the patch's
// own send effects, with the original's names and display texts (param_tables.h, FxText.h). It follows the Schwung
// module's editor (schwung/src/hs2_module.cpp): the same pages, names, texts and steps.
//
// An edit address is (element, index):
//   element kSendFxElem (-1): the part's patch send effects, engine part parameter j = 56 + slot * 24 + j' (index = j)
//   element 0: the patch, patch parameter index 0..255
//   element 1..64: an element of the patch (setter index = element record + 1), parameter index 0..255
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
bool editReadOnly(const EditContext& c, int elem, int idx);  // Element Type and an insert's Effect Type
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
