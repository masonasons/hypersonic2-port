#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace hs2port {
#if JUCE_WINDOWS
bool announceTypingSetting(juce::Component& focus, const juce::String& text);
#else
inline bool announceTypingSetting(juce::Component&, const juce::String& text) {
    juce::AccessibilityHandler::postAnnouncement(text, juce::AccessibilityHandler::AnnouncementPriority::medium);
    return true;
}
#endif
}
