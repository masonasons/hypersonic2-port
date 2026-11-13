#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace hs2port {

inline void setControlAccelerator(juce::Component& control, const char* shortcut) {
#if JUCE_WINDOWS
    control.getProperties().set("hs2AcceleratorKey", shortcut);
#else
    juce::ignoreUnused(control, shortcut);
#endif
}

inline void setControlMnemonic(juce::Component& control, int key) {
#if JUCE_WINDOWS
    control.getProperties().set("hs2AccessKey",
                                "Alt+" + juce::String::charToString(juce::CharacterFunctions::toUpperCase(
                                             juce::juce_wchar(key))));
#else
    juce::ignoreUnused(control, key);
#endif
}

}  // namespace hs2port
