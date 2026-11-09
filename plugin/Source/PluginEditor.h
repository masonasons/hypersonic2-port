#pragma once
// HS2 Port editor, designed first for screen-reader use (NVDA / JAWS / Narrator through UI Automation on Windows,
// VoiceOver on Mac). Rules followed here:
//  * Only stock JUCE widgets, which have accessibility handlers; every one gets a title (what the screen reader says),
//    a description and a fixed keyboard focus order. Nothing is conveyed by colour or graphics alone.
//  * Controls are grouped in titled sections (accessibility role "group") in a logical order:
//    Part, Patch browser, Current patch, Hyperknobs, Part mixer, Master, Settings, Status. The sections that work on
//    the selected part carry its number in their title ("Part 3 Hyperknobs").
//  * No spoken announcements (JUCE's postAnnouncement uses its own SAPI voice on Windows, not the screen reader).
//    Information is text the screen reader reads in the normal way: the part selector names each part's patch, the
//    patch list's name includes the number of patches shown, status is a read-only text box, and when an action the
//    user asked for fails, focus moves there.
//  * Keyboard: Ctrl+P part selector, Ctrl+Shift+Page Up / Page Down previous / next part, Ctrl+F search, Ctrl+L
//    patch list, Ctrl+I patch information, Ctrl+Page Up / Page Down previous / next patch, F1 keyboard help. In the
//    list: type letters to jump to a patch, Enter to load it.
#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "PluginProcessor.h"

namespace hs2port {

// A titled group of controls (accessibility role "group").
class Section : public juce::Component {
public:
    explicit Section(const juce::String& title);
    void paint(juce::Graphics& g) override;
    juce::Rectangle<int> content() const { return getLocalBounds().reduced(10).withTrimmedTop(22); }

private:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
};

// ListBox with type-ahead: typed letters jump to the next patch whose name starts with them.
// Windows screen readers: JUCE gives the keyboard focus to the list itself, so NVDA sometimes announces only the list
// and not the selected patch. The list hands the accessibility focus to the selected row (a named item) when it gets
// the focus and when the selection changes (the technique of OnjLouis/sound-canvas-bridge, MIT licence).
class PatchListBox : public juce::ListBox {
public:
    std::function<juce::String(int)> rowName;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType cause) override;
    void focusSelectedRow();

private:
    juce::String prefix_;
    juce::uint32 lastKeyTime_ = 0;
};

class HS2Editor : public juce::AudioProcessorEditor, private juce::ListBoxModel, private juce::Timer {
public:
    explicit HS2Editor(HS2Processor&);
    ~HS2Editor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void visibilityChanged() override;
    void focusGained(FocusChangeType) override;

private:
    // ListBoxModel
    int getNumRows() override { return int(filtered_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    juce::String getNameForRow(int row) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void returnKeyPressed(int row) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

    void timerCallback() override;
    void refreshFromProcessor();
    void refreshPartBox();
    void selectPart(int part);  // attaches the part's controls and shows its patch
    void rebuildBankBox();
    void rebuildList();
    void selectCurrentPatchInList();
    void loadRow(int row, bool explicitRequest);
    void stepPatch(int delta);
    void chooseContentFolder();
    void showKeyboardHelp();
    void confirmInstallPresets();
    void showError(const juce::String& text);  // status text + focus there, for failures of user actions
    juce::String infoText(int part, const PatchInfo& inf) const;
    juce::String rowText(int row) const;
    int part() const { return proc_.selectedPart.load(); }

    HS2Processor& proc_;
    std::shared_ptr<const PatchLibrary> library_;
    std::vector<int> filtered_;  // indices into library_->patches
    int seenChange_ = -1;
    int attachedPart_ = -1;
    bool suppressSelectionLoad_ = false;
    bool focusedOnce_ = false;

    Section partSection_{"Part"}, browser_{"Patch browser"}, current_{"Current patch"}, knobs_{"Hyperknobs"},
        mixer_{"Part mixer"}, master_{"Master"}, settings_{"Settings"}, statusSection_{"Status"};

    // Part
    juce::Label partLabel_;
    juce::ComboBox partBox_;
    juce::TextButton clearPart_{"Clear part"};

    // Patch browser
    juce::Label searchLabel_, bankLabel_;
    juce::TextEditor search_;
    juce::ComboBox bankBox_;
    PatchListBox list_;
    juce::TextButton loadButton_{"Load selected patch"}, prevButton_{"Previous patch"}, nextButton_{"Next patch"};
    juce::ToggleButton autoLoad_{"Load patch when selected in the list"};

    juce::TextEditor info_;

    // Hyperknobs and mixer of the selected part
    std::array<juce::Slider, kNumKnobs> knobSliders_;
    std::array<juce::Label, kNumKnobs> knobLabels_;
    std::array<std::unique_ptr<juce::SliderParameterAttachment>, kNumKnobs> knobAttach_;
    juce::ToggleButton mute_{"Mute"};
    std::unique_ptr<juce::ButtonParameterAttachment> muteAttach_;
    juce::Slider level_, pan_;
    std::array<juce::Slider, kNumSends> sends_;
    juce::Label levelLabel_, panLabel_;
    std::array<juce::Label, kNumSends> sendLabels_;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttach_, panAttach_;
    std::array<std::unique_ptr<juce::SliderParameterAttachment>, kNumSends> sendAttach_;

    // Master
    juce::Slider masterVolume_;
    juce::Label masterLabel_;
    std::unique_ptr<juce::SliderParameterAttachment> masterAttach_;

    // Settings
    juce::Label contentLabel_, memoryLabel_;
    juce::TextEditor contentPath_;
    juce::TextButton chooseContent_{"Choose content folder..."};
    juce::ComboBox memoryMode_;
    juce::ToggleButton midiPrograms_{"Load patches and combis from MIDI program changes"};
    juce::ToggleButton ringOut_{"Let effect tails ring out"};
    juce::ToggleButton cutOnLoad_{"Stop the old sound when a part's patch changes"};
    juce::TextButton audioSettings_{"Audio and MIDI settings..."};  // standalone app only
    juce::TextButton installPresets_{"Install presets..."};
    juce::TextButton help_{"Keyboard help"};

    juce::TextEditor status_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HS2Editor)
};

}  // namespace hs2port
