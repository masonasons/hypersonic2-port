#pragma once
#include <juce_gui_extra/juce_gui_extra.h>

#if JUCE_MAC
namespace hs2port {
std::unique_ptr<juce::SystemTrayIconComponent> createMacMenuBarItem(std::function<void()> show,
                                                                  std::function<void()> preferences);
void setMacMenuBarHidden(bool hidden);
}
#endif
