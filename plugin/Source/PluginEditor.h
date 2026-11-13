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

#if JUCE_WINDOWS
namespace juce { class SystemTrayIconComponent; }
#endif

#include <memory>
#include <vector>

#include "PluginProcessor.h"
#include "ReviewText.h"
#include "AccessibleToggle.h"
#include "KeyboardChoice.h"
#include "MusicalTyping.h"

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

// A slider with the keys the keyboard help lists. JUCE's slider handles only the arrow keys; this adds Page Up / Page
// Down (a tenth of the range), Home / End (lowest / highest) and Delete or Backspace (back to defaultValue: the value
// the part's patch loaded with). Keys with modifiers go on to the editor (Ctrl+Page Up / Down change the patch).
class KeySlider : public juce::Slider {
public:
    KeySlider();
    ~KeySlider() override;
    std::function<double()> defaultValue;
    // Optional: the arrow keys step to the value `delta` texts away (Page Up / Down: ten), instead of a fixed amount.
    std::function<double(double value, int delta)> stepper;
    std::function<void()> onReset;  // optional: Delete / Backspace call this instead of going to defaultValue
    bool keyPressed(const juce::KeyPress& key) override;
    void resized() override;
    bool isEntering() const { return editing_; }
private:
    bool commitEntry();
    void finishEntry(bool commit, bool focus);
    void applyKeyboardValue(double value);
    class Entry final : public juce::TextEditor {
    public:
        std::function<void(bool)> navigate;
        std::function<bool(const juce::KeyPress&)> step;
        bool keyPressed(const juce::KeyPress& key) override {
            const auto mods = key.getModifiers();
            if (key.isKeyCode(juce::KeyPress::tabKey) && !mods.isAltDown() && !mods.isCommandDown() && !mods.isCtrlDown()) {
                navigate(!mods.isShiftDown());
                return true;
            }
            if (!mods.isAnyModifierKeyDown() &&
                (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)
                 || key.isKeyCode(juce::KeyPress::pageUpKey) || key.isKeyCode(juce::KeyPress::pageDownKey)))
                return step(key);
            return TextEditor::keyPressed(key);
        }
    };
    Entry entry_;
    juce::String initialEntry_;
    bool editing_ = false;
};

// One setting whose value comes from callbacks instead of a host parameter: the patch editor (PatchEdit.h) and the
// global settings. A choice box when it has a list of values, else a slider (stepping by the value texts).
class ValueRow : public juce::Component {
public:
    struct Binding {
        juce::String title, description;
        std::function<float()> get;
        std::function<void(float)> set;
        std::function<juce::String(float)> text;
        std::function<float(float, int)> step;                // arrow keys: the value `delta` texts away
        std::function<void()> reset;                          // Delete / Backspace (none: not offered)
        std::vector<std::pair<float, juce::String>> choices;  // non-empty: a choice box
        bool readOnly = false;
        juce::String key;  // what the row shows (the editor rebinds a row only when this, the title or choices change)
    };
    explicit ValueRow(int order);
    void bind(Binding b);  // shows the row
    void unbind();         // hides it
    bool bound() const { return bound_; }
    const Binding& binding() const { return b_; }
    void refresh();        // shows the current value
    void resized() override;

private:
    void showValue(float v);
    Binding b_;
    bool bound_ = false, updating_ = false;
    juce::String shownText_;  // the slider's text as last shown
    juce::Label label_;
    KeyboardChoice box_;
    KeySlider slider_;
};

// One setting on a page: a label (hidden from screen readers; the control carries the title) and either a choice box
// (a setting with a fixed list of values, e.g. Transpose, Arp Mode) or a slider (Fine Tune, effect knobs), bound to
// an exposed host parameter. bind() moves it to another part or effect slot.
class ParamRow : public juce::Component {
public:
    ParamRow(HS2Processor& p, const juce::String& title, const juce::String& description, int order);
    void bind(int exposedIndex);  // -1: unbound
    void setRowTitle(const juce::String& title, const juce::String& description);
    int index() const { return index_; }
    void resized() override;

private:
    void showValue(float v);
    HS2Processor& proc_;
    int index_ = -1;
    juce::Label label_;
    KeyboardChoice box_;
    KeySlider slider_;
    std::vector<std::pair<float, juce::String>> choices_;
    std::unique_ptr<juce::ParameterAttachment> boxAttach_;
    std::unique_ptr<juce::SliderParameterAttachment> sliderAttach_;
    bool updating_ = false;
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
    void focusOfChildComponentChanged(FocusChangeType) override;

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
    // The pages of the right side, in the Show box's order.
    enum Page { pgPatch, pgSettings, pgArp, pgTuning, pgPatchParams, pgElements, pgSends, pgGfx, pgGlobal, pgInfo, kNumPages };
    void setPage(int page, bool focusSelector);
    void bindPages();                            // the page rows to the selected part and effect slot
    void focusPage();                            // keyboard focus to the shown page's first control
    void refreshPages();                         // effect slot names, tuning scale lists
    // The patch editor pages (patch parameters, elements, patch send effects) from the part's latest values.
    void refreshEditPages(bool rebindAll);
    void unbindEditRows();
    void bindEditRows(std::vector<std::unique_ptr<ValueRow>>& rows, int elem, const std::vector<int>& idx, bool rebindAll);
    static void setBoxItems(juce::ComboBox& box, const juce::StringArray& items, int selectedIndex);
    // Rows in two columns, after head controls each on its own line (control, its label).
    static void layoutRows(Section& s, const std::vector<juce::Component*>& rows,
                           const std::vector<std::pair<juce::Component*, juce::Label*>>& heads);
    void fillEffectBox();  // the Effect box: the content's effect presets, the selected slot's effect selected
    void rebuildBankBox();
    void rebuildList();
    void selectCurrentPatchInList();
    void loadRow(int row, bool explicitRequest);
    void stepPatch(int delta);
    void chooseContentFolder();
    void showKeyboardHelp(bool contextual = false);
    bool handleMnemonic(int key);
    enum class View { instrument, general, audio, help };
    void showView(View view);
    void configureStandaloneWindow();
    void hideToTray();
    void restoreFromTray();
    void checkForUpdates();
    void toggleKeyboard();
    bool playKey(const juce::KeyPress&);
    void refreshKeyboard();
    void sendTypingNote(int channel, int pitch, int velocity);
    void confirmInstallPresets(bool upgrade = false);
    void offerPresetUpgrade();
    bool checkedPresetUpgrade_ = false;
    void choosePresetFolder(PresetDestination format);
    void refreshPresetFolders();
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
    KeyboardChoice partBox_;
    juce::TextButton clearPart_{"Clear part"};

    // Patch browser
    juce::Label searchLabel_, bankLabel_;
    juce::TextEditor search_;
    KeyboardChoice bankBox_;
    PatchListBox list_;
    juce::TextButton loadButton_{"Load selected patch"}, prevButton_{"Previous patch"}, nextButton_{"Next patch"};
    AccessibleToggle autoLoad_{"Load patch when selected in the list"};

    ReviewText info_;

    // Hyperknobs and mixer of the selected part
    std::array<KeySlider, kNumKnobs> knobSliders_;
    std::array<juce::Label, kNumKnobs> knobLabels_;
    std::array<std::unique_ptr<juce::SliderParameterAttachment>, kNumKnobs> knobAttach_;
    AccessibleToggle mute_{"Mute"};
    std::unique_ptr<juce::ButtonParameterAttachment> muteAttach_;
    KeySlider level_, pan_;
    std::array<KeySlider, kNumSends> sends_;
    juce::Label levelLabel_, panLabel_;
    std::array<juce::Label, kNumSends> sendLabels_;
    std::unique_ptr<juce::SliderParameterAttachment> levelAttach_, panAttach_;
    std::array<std::unique_ptr<juce::SliderParameterAttachment>, kNumSends> sendAttach_;

    // Pages beyond the patch and mixer: the selected part's settings, arpeggiator and microtuning, the global effects
    Section settingsPage_{"Part settings"}, arpPage_{"Arpeggiator"}, tuningPage_{"Microtuning"},
        gfxPage_{"Global effects"};
    juce::Label pageLabel_, gfxSlotLabel_, gfxEffectLabel_;
    KeyboardChoice pageBox_, gfxSlot_, gfxEffect_;
    std::vector<juce::String> effectPaths_;  // Effect box item id 3 + i: PatchLibrary::effects[i] (path under Effects)
    juce::String seenEffectSig_;
    std::vector<std::unique_ptr<ParamRow>> settingsRows_, arpRows_, tuningRows_, gfxRows_;
    std::vector<int> settingsE_, arpE_, tuningE_, gfxJ_;  // ParamDefs index (kExtDefs e / global effect j) per row
    std::unique_ptr<ParamRow> linkRow_, keepRow_, trimRow_;
    std::unique_ptr<ParamRow> gfxSend1Row_;  // shown for effects 2..4 (they feed effect 1)
    std::vector<const void*> gfxKnobSeen_;   // the knob table each effect knob row shows (rebind when it changes)
    int page_ = 0;
    std::array<int, 2> seenTuneBank_{{-1, -1}};
    juce::StringArray seenFxNames_;

    // The patch editor pages of the selected part
    Section patchPage_{"Patch parameters"}, elemPage_{"Elements"}, sendsPage_{"Patch send effects"},
        globalPage_{"Global settings"};
    juce::Label patchGroupLabel_, elemLabel_, elemGroupLabel_, sendSlotLabel_;
    KeyboardChoice patchGroupBox_, elemBox_, elemGroupBox_, sendSlotBox_;
    std::vector<std::unique_ptr<ValueRow>> patchRows_, elemRows_, sendRows_, globalRows_;
    EditContext ctx_;                   // the texts' view of snap_ and editsSeen_
    PartSnapshot snap_;                 // the selected part's values (HS2Processor::snapshot)
    EditMap editsSeen_;                 // its edits
    std::shared_ptr<hs2::Content> content_;
    int snapSerial_ = -1, snapPart_ = -1;
    int patchGroup_ = 0, elemIndex_ = 0, elemGroup_ = 0, sendSlot_ = 0;  // what the boxes show
    juce::StringArray patchGroupTitles_, elemTitles_, elemGroupTitles_, sendSlotTitles_;

    // Master
    KeySlider masterVolume_;
    juce::Label masterLabel_;
    std::unique_ptr<juce::SliderParameterAttachment> masterAttach_;

    // Settings
    juce::Label contentLabel_, memoryLabel_;
    juce::TextEditor contentPath_;
    juce::TextButton chooseContent_{"Choose content folder..."};
    KeyboardChoice memoryMode_;
    AccessibleToggle midiPrograms_{"Load patches and combis from MIDI program changes"};
    AccessibleToggle ringOut_{"Let effect tails ring out"};
    AccessibleToggle cutOnLoad_{"Stop the old sound when a part's patch changes"};
    juce::TextButton installPresets_{"Install presets..."};
    struct PresetFolderRow {
        juce::Label label;
        juce::TextEditor path;
        juce::TextButton browse{"Browse..."}, reset{"Use default"};
    };
    PresetFolderRow vst3Folder_, nksFolder_;
    juce::TextButton allNotesOff_{"All notes off"};
    juce::TextButton rescan_{"Rescan content folder"};
    juce::TextButton help_{"Keyboard help"};

    ReviewText status_, helpText_;
    juce::TextButton preferences_{"Preferences"}, hide_{"Hide to tray"}, back_{"Back to instrument"},
        generalTab_{"General"}, audioTab_{"Audio and MIDI"}, updates_{"Releases and updates..."}, closeHelp_{"Close help"};
    juce::Component preferencesBar_, helpPane_;
    std::unique_ptr<juce::Component> audioPanel_;
    std::unique_ptr<juce::SystemTrayIconComponent> tray_;
    juce::Component::SafePointer<juce::Component> returnFocus_;
    View view_ = View::instrument, helpReturn_ = View::instrument;
    bool nativeWindowConfigured_ = false;
    bool checkingUpdates_ = false;
    AccessibleToggle keyboard_{"Musical Keyboard"};
    KeySlider octave_, velocity_;
    juce::Label octaveLabel_, velocityLabel_;
    Section keyboardPanel_{"Musical Keyboard"};
    MusicalTyping typing_;
    std::array<bool, typingKeys.size()> blockedTypingKeys_{};
    bool typingActive_ = false;
    class SoundButton final : public juce::TextButton {
    public:
        std::function<bool(const juce::KeyPress&)> navigate;
        bool keyPressed(const juce::KeyPress& key) override {
            return (navigate && navigate(key)) || TextButton::keyPressed(key);
        }
    };
    SoundButton currentSound_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HS2Editor)
};

}  // namespace hs2port
