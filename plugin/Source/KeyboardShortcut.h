#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace hs2port {
inline char mnemonicLetter(const juce::KeyPress& key) {
    const auto code = key.getKeyCode();
    if (code >= 'A' && code <= 'Z') return char(code - 'A' + 'a');
    if (code >= 'a' && code <= 'z') return char(code);
    return 0;
}
}
