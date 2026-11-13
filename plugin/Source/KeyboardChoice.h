#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace hs2port {
// Keyboard choice behavior follows Sound Canvas Bridge (OnjMusic, MIT).
class KeyboardChoice : public juce::ComboBox {
public:
    bool keyPressed(const juce::KeyPress& key) override {
        using Key = juce::KeyPress;
        if (!isEnabled() || key.getModifiers().isAnyModifierKeyDown()) return false;
        const int code = key.getKeyCode();
        constexpr int pageStep = 10;
        int next = getSelectedItemIndex();
        if (code == Key::upKey || code == Key::leftKey) --next;
        else if (code == Key::downKey || code == Key::rightKey) ++next;
        else if (code == Key::pageUpKey) next -= pageStep;
        else if (code == Key::pageDownKey) next += pageStep;
        else if (code == Key::homeKey) next = 0;
        else if (code == Key::endKey) next = getNumItems() - 1;
        else if (code == Key::returnKey) { showPopup(); return true; }
        else return ComboBox::keyPressed(key);
        if (getNumItems() > 0) {
            const auto before = getText();
            setSelectedItemIndex(juce::jlimit(0, getNumItems() - 1, next), juce::sendNotificationSync);
            if (before != getText()) {
                if (auto* handler = getAccessibilityHandler())
                    handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
#if JUCE_MAC
                juce::AccessibilityHandler::postAnnouncement(getTitle() + ", " + getText(),
                    juce::AccessibilityHandler::AnnouncementPriority::medium);
#endif
            }
        }
        return true;
    }
};
}
