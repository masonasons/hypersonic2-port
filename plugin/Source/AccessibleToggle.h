#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace hs2port {
// Match Sound Canvas Bridge: expose the Windows Toggle action without a read-only Value pattern.
class AccessibleToggle : public juce::ToggleButton {
public:
    using juce::ToggleButton::ToggleButton;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
#if JUCE_WINDOWS
        class Handler final : public juce::AccessibilityHandler {
        public:
            explicit Handler(AccessibleToggle& control)
                : AccessibilityHandler(control, juce::AccessibilityRole::toggleButton,
                      juce::AccessibilityActions()
                          .addAction(juce::AccessibilityActionType::press, [&control] {
                              if (control.isEnabled()) control.triggerClick();
                          })
                          .addAction(juce::AccessibilityActionType::toggle, [&control] {
                              if (control.isEnabled()) control.setToggleState(!control.getToggleState(), juce::sendNotification);
                          })), button(control) {}
            juce::AccessibleState getCurrentState() const override {
                auto state = AccessibilityHandler::getCurrentState().withCheckable();
                return button.getToggleState() ? state.withChecked() : state;
            }
            juce::String getTitle() const override {
                const auto title = AccessibilityHandler::getTitle();
                return title.isNotEmpty() ? title : button.getButtonText();
            }
        private:
            AccessibleToggle& button;
        };
        return std::make_unique<Handler>(*this);
#else
        return juce::ToggleButton::createAccessibilityHandler();
#endif
    }
};
}
