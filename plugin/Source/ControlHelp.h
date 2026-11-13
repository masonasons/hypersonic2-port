#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace hs2port {
inline constexpr auto kContextHelp = "hs2ContextHelp";

inline void setControlHelp(juce::Component& control, const juce::String& help) {
    control.getProperties().set(kContextHelp, help);
}

inline juce::String controlHelp(const juce::Component* control, const juce::Component& root) {
    for (; control != nullptr && control != &root; control = control->getParentComponent()) {
        const auto help = control->getProperties()[kContextHelp].toString();
        if (help.isNotEmpty()) return help;
    }
    return {};
}
}
