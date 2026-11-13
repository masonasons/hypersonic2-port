#include "PluginEditor.h"
#include "ControlHelp.h"
#include "ControlShortcuts.h"
#include "KeyboardNumber.h"
#include "KeyboardShortcut.h"
#include "TypingAnnouncement.h"

#include "AudioSettings.h"
#include "MacMenuBar.h"
#include "PresetExport.h"
#include "hs2/effects.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <cmath>
#include <optional>
#include <tuple>

namespace hs2port {

// A font of `height` in both JUCE 9 (FontOptions) and JUCE 7 (the Windows 7 build).
static juce::Font uiFont(float height, int style = juce::Font::plain) {
#if JUCE_MAJOR_VERSION >= 8
    return juce::Font(juce::FontOptions(height, style));
#else
    return juce::Font(height, style);
#endif
}

namespace {
const juce::Colour kBackground(0xff1e2126), kPanel(0xff2a2e35), kText(0xfff2f2f2), kAccent(0xff8fc6ff);
constexpr auto kReleases = "https://github.com/masonasons/hypersonic2-port/releases";
constexpr auto kLatestRelease = "https://api.github.com/repos/masonasons/hypersonic2-port/releases/latest";

std::optional<std::array<int, 3>> releaseVersion(juce::String version) {
    const auto fields = juce::StringArray::fromTokens(version, ".", {});
    if (fields.size() != 3) return {};
    std::array<int, 3> result{};
    for (int i = 0; i < 3; ++i) {
        if (fields[i].isEmpty() || fields[i].length() > 6 || !fields[i].containsOnly("0123456789")) return {};
        result[size_t(i)] = fields[i].getIntValue();
    }
    return result;
}

class TrayIcon final : public juce::SystemTrayIconComponent {
public:
    TrayIcon(std::function<void()> show, std::function<void()> quit)
        : show_(std::move(show)), quit_(std::move(quit)) {
        juce::Image image(juce::Image::ARGB, 32, 32, true);
        juce::Graphics g(image);
        g.setColour(kPanel);
        g.fillRoundedRectangle(1, 1, 30, 30, 4);
        g.setColour(kAccent);
        for (int i = 0; i < 3; ++i) g.fillRect(6 + 8 * i, 7 + 4 * i, 4, 18 - 4 * i);
        setIconImage(image, image);
        setIconTooltip("HS2 Port");
    }
    void mouseDown(const juce::MouseEvent& event) override {
        if (!event.mods.isPopupMenu()) {
            show_();
            return;
        }
        juce::PopupMenu menu;
        menu.addItem(1, "Show HS2 Port");
        menu.addItem(2, "Quit HS2 Port");
        menu.showMenuAsync({}, [safe = juce::Component::SafePointer<TrayIcon>(this)](int result) {
            if (safe && result == 1) safe->show_();
            if (safe && result == 2) safe->quit_();
        });
    }
private:
    std::function<void()> show_, quit_;
};

void setupLabel(juce::Label& l, const juce::String& text, juce::Component& target) {
    l.setText(text, juce::dontSendNotification);
    l.setColour(juce::Label::textColourId, kText);
    l.attachToComponent(&target, true);
    l.setAccessible(false);  // visual only: the control itself carries the same title, so it is not read twice
}

void setupReadOnly(juce::TextEditor& t, const juce::String& title, const juce::String& description, bool multiLine) {
    t.setMultiLine(multiLine, true);
    t.setReadOnly(true);
    t.setCaretVisible(true);  // so screen-reader users can move through the text by line and word
    t.setScrollbarsShown(true);
    t.setTitle(title);
    setControlHelp(t, description);
    t.setWantsKeyboardFocus(true);
}

void setupSlider(juce::Slider& s, int order, const juce::String& description) {
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, true, 110, 22);  // read-only box: no extra unnamed tab stop
    s.setExplicitFocusOrder(order);
    s.setWantsKeyboardFocus(true);
    setControlHelp(s, description);
}

const char* kSliderKeys = "Arrow keys step; Page Up and Page Down jump ten steps. Home and End go to the limits. "
                          "Enter edits the normalized value. Delete restores the patch setting.";
const char* kSliderKeysNoReset = "Arrow keys change the value; Page Up and Page Down in larger steps; Home and End go "
                                 "to the lowest and highest value; Delete returns to the default.";
}  // namespace

// ------------------------------------------------------------------------------------------------ KeySlider

KeySlider::KeySlider() {
    addChildComponent(entry_);
    entry_.setTabKeyUsedAsCharacter(false);
    entry_.setSelectAllWhenFocused(true);
    entry_.onReturnKey = [this] { if (commitEntry()) finishEntry(false, true); };
    entry_.onEscapeKey = [this] { finishEntry(false, true); };
    entry_.onFocusLost = [this] { finishEntry(true, false); };
    entry_.navigate = [this](bool forward) {
        finishEntry(true, false);
        moveKeyboardFocusToSibling(forward);
    };
    entry_.step = [this](const juce::KeyPress& key) {
        if (commitEntry()) {
            keyPressed(key);
            initialEntry_ = juce::String(getValue(), getInterval() == 1 ? 0 : 6).trim();
            entry_.setText(initialEntry_, false);
            entry_.selectAll();
        }
        return true;
    };
}

KeySlider::~KeySlider() {
    editing_ = false;
    entry_.onFocusLost = nullptr;
}

void KeySlider::resized() {
    Slider::resized();
    entry_.setBounds(getLocalBounds().removeFromRight(getTextBoxWidth()));
    for (auto* child : getChildren()) if (auto* label = dynamic_cast<juce::Label*>(child)) {
        label->setAccessible(false);
        label->setWantsKeyboardFocus(false);
    }
}

void KeySlider::applyKeyboardValue(double value) {
    const auto before = getValue();
    {
        juce::Slider::ScopedDragNotification gesture(*this);
        setValue(juce::jlimit(getMinimum(), getMaximum(), value), juce::sendNotificationSync);
    }
    if (getValue() == before) return;
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

bool KeySlider::commitEntry() {
    const auto value = entry_.getText().trim();
    if (value == initialEntry_) return true;
    const auto number = keyboardNumber(value.toStdString());
    if (!number || *number < getMinimum() || *number > getMaximum()) {
        entry_.selectAll();
        return false;
    }
    applyKeyboardValue(*number);
    return true;
}

void KeySlider::finishEntry(bool commit, bool focus) {
    if (!editing_) return;
    editing_ = false;
    if (commit) commitEntry();
    entry_.setVisible(false);
    if (focus) grabKeyboardFocus();
}

bool KeySlider::keyPressed(const juce::KeyPress& key) {
    if (!key.getModifiers().isAnyModifierKeyDown()) {
        const double lo = getMinimum(), hi = getMaximum();
        const int code = key.getKeyCode();
        if (code == juce::KeyPress::returnKey) {
            initialEntry_ = juce::String(getValue(), getInterval() == 1 ? 0 : 6).trim();
            entry_.setTitle(getTitle() + " value");
            entry_.setText(initialEntry_, false);
            editing_ = true;
            entry_.setVisible(true);
            entry_.toFront(false);
            entry_.grabKeyboardFocus();
            entry_.selectAll();
            return true;
        }
        if (stepper) {  // step by the value texts
            int delta = 0;
            if (code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey) delta = 1;
            else if (code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey) delta = -1;
            else if (code == juce::KeyPress::pageUpKey) delta = 10;
            else if (code == juce::KeyPress::pageDownKey) delta = -10;
            if (delta != 0) {
                applyKeyboardValue(stepper(getValue(), delta));
                return true;
            }
        }
        if ((code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) && onReset) {
            onReset();
            return true;
        }
        double v = getValue();
        bool handled = true;
        const auto step = getInterval() > 0 ? getInterval() : (hi - lo) / 100.0;
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey) v += step;
        else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey) v -= step;
        else if (code == juce::KeyPress::pageUpKey) v += step * 10;
        else if (code == juce::KeyPress::pageDownKey) v -= step * 10;
        else if (code == juce::KeyPress::homeKey) v = lo;
        else if (code == juce::KeyPress::endKey) v = hi;
        else if ((code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) && defaultValue) v = defaultValue();
        else handled = false;
        if (handled) {
            applyKeyboardValue(v);
            return true;
        }
    }
    return juce::Slider::keyPressed(key);
}

// ------------------------------------------------------------------------------------------------ ParamRow

ParamRow::ParamRow(HS2Processor& p, const juce::String& title, const juce::String& description, int order) : proc_(p) {
    setExplicitFocusOrder(order);
    label_.setText(title, juce::dontSendNotification);
    label_.setAccessible(false);  // the control carries the same title
    addAndMakeVisible(label_);
    box_.setTitle(title);
    setControlHelp(box_, description + " Up and Down arrows choose a value.");
    box_.setExplicitFocusOrder(1);
    box_.setWantsKeyboardFocus(true);
    addChildComponent(box_);
    setupSlider(slider_, 1, description + " " + kSliderKeys);
    slider_.setTitle(title);
    addChildComponent(slider_);
    box_.onChange = [this] {
        if (updating_ || boxAttach_ == nullptr) return;
        const int k = box_.getSelectedItemIndex();
        if (k >= 0 && k < int(choices_.size())) boxAttach_->setValueAsCompleteGesture(choices_[size_t(k)].first);
    };
}

void ParamRow::setRowTitle(const juce::String& title, const juce::String& description) {
    label_.setText(title, juce::dontSendNotification);
    box_.setTitle(title);
    setControlHelp(box_, description + " Up and Down arrows choose a value.");
    slider_.setTitle(title);
    setControlHelp(slider_, description + " " + kSliderKeys);
}

void ParamRow::bind(int i) {
    index_ = i;
    boxAttach_.reset();
    sliderAttach_.reset();
    if (i < 0) {
        box_.setVisible(false);
        slider_.setVisible(false);
        return;
    }
    auto& param = proc_.exposed(i);
    choices_ = proc_.choices(i);
    if (!choices_.empty()) {
        slider_.setVisible(false);
        box_.setVisible(true);
        updating_ = true;
        box_.clear(juce::dontSendNotification);
        for (size_t k = 0; k < choices_.size(); ++k)
            box_.addItem(choices_[k].second.isNotEmpty() ? choices_[k].second : juce::String("(none)"), int(k) + 1);
        updating_ = false;
        boxAttach_ = std::make_unique<juce::ParameterAttachment>(param, [this](float v) { showValue(v); });
        boxAttach_->sendInitialUpdate();
    } else {
        box_.setVisible(false);
        slider_.setVisible(true);
        sliderAttach_ = std::make_unique<juce::SliderParameterAttachment>(param, slider_);
        slider_.textFromValueFunction = [this](double v) { return proc_.displayText(index_, float(v)); };
        slider_.defaultValue = [this] { return double(proc_.loadedValue(index_)); };
        slider_.updateText();
    }
}

// The choice that shows v: the one whose text is v's text, else the nearest value.
void ParamRow::showValue(float v) {
    const auto text = proc_.displayText(index_, v);
    int best = -1;
    for (size_t k = 0; k < choices_.size() && best < 0; ++k)
        if (choices_[k].second == text) best = int(k);
    if (best < 0) {
        float bestD = 2.f;
        for (size_t k = 0; k < choices_.size(); ++k)
            if (std::abs(choices_[k].first - v) < bestD) {
                bestD = std::abs(choices_[k].first - v);
                best = int(k);
            }
    }
    updating_ = true;
    box_.setSelectedItemIndex(best, juce::dontSendNotification);
    updating_ = false;
}

void ParamRow::resized() {
    auto r = getLocalBounds();
    label_.setBounds(r.removeFromLeft(juce::jmin(150, r.getWidth() * 45 / 100)));
    box_.setBounds(r);
    slider_.setBounds(r);
}

// ------------------------------------------------------------------------------------------------ ValueRow

ValueRow::ValueRow(int order) {
    setExplicitFocusOrder(order);
    label_.setAccessible(false);  // the control carries the same title
    addAndMakeVisible(label_);
    box_.setExplicitFocusOrder(1);
    box_.setWantsKeyboardFocus(true);
    addChildComponent(box_);
    setupSlider(slider_, 1, {});
    slider_.setRange(0.0, 1.0, 0.0);
    addChildComponent(slider_);
    box_.onChange = [this] {
        if (updating_ || !bound_ || b_.readOnly || !b_.set) return;
        const int k = box_.getSelectedItemIndex();
        if (k >= 0 && k < int(b_.choices.size())) b_.set(b_.choices[size_t(k)].first);
    };
    slider_.onValueChange = [this] {
        if (!updating_ && bound_ && !b_.readOnly && b_.set) b_.set(float(slider_.getValue()));
    };
    slider_.textFromValueFunction = [this](double v) { return bound_ && b_.text ? b_.text(float(v)) : juce::String(); };
    slider_.stepper = [this](double v, int delta) {
        return bound_ && b_.step ? double(b_.step(float(v), delta)) : juce::jlimit(0.0, 1.0, v + 0.01 * delta);
    };
    slider_.onReset = [this] {
        if (bound_ && b_.reset) {
            b_.reset();
            refresh();
        }
    };
    setVisible(false);
}

void ValueRow::bind(Binding b) {
    b_ = std::move(b);
    bound_ = true;
    label_.setText(b_.title, juce::dontSendNotification);
    if (!b_.choices.empty()) {
        box_.setTitle(b_.title);
        setControlHelp(box_, b_.description + (b_.readOnly ? " Read only: the patch sets it."
                                                          : " Up and Down arrows choose a value."));
        updating_ = true;
        box_.clear(juce::dontSendNotification);
        for (size_t k = 0; k < b_.choices.size(); ++k)
            box_.addItem(b_.choices[k].second.isNotEmpty() ? b_.choices[k].second : juce::String("(none)"), int(k) + 1);
        updating_ = false;
        slider_.setVisible(false);
        box_.setVisible(true);
    } else {
        slider_.setTitle(b_.title);
        setControlHelp(slider_, b_.description + " " + (b_.reset ? kSliderKeys : kSliderKeysNoReset));
        box_.setVisible(false);
        slider_.setVisible(true);
    }
    setVisible(true);
    refresh();
}

void ValueRow::unbind() {
    bound_ = false;
    shownText_.clear();
    b_ = Binding();
    setVisible(false);
}

void ValueRow::refresh() {
    if (!bound_ || !b_.get) return;
    showValue(b_.get());
}

// The choice that shows v: the one whose text is v's text, else the nearest value. A slider: the value (and its text,
// which can change with other values, e.g. a delay time that follows Sync).
void ValueRow::showValue(float v) {
    updating_ = true;
    if (!b_.choices.empty()) {
        const auto text = b_.text ? b_.text(v) : juce::String();
        int best = -1;
        for (size_t k = 0; k < b_.choices.size() && best < 0; ++k)
            if (b_.choices[k].second == text) best = int(k);
        if (best < 0) {
            float bestD = 2.f;
            for (size_t k = 0; k < b_.choices.size(); ++k)
                if (std::abs(b_.choices[k].first - v) < bestD) {
                    bestD = std::abs(b_.choices[k].first - v);
                    best = int(k);
                }
        }
        if (box_.getSelectedItemIndex() != best) box_.setSelectedItemIndex(best, juce::dontSendNotification);
    } else {
        if (std::abs(slider_.getValue() - double(v)) > 1e-7) slider_.setValue(double(v), juce::dontSendNotification);
        if (const auto t = b_.text ? b_.text(v) : juce::String(); t != shownText_) {
            shownText_ = t;
            slider_.updateText();
        }
    }
    updating_ = false;
}

void ValueRow::resized() {
    auto r = getLocalBounds();
    label_.setBounds(r.removeFromLeft(juce::jmin(150, r.getWidth() * 45 / 100)));
    box_.setBounds(r);
    slider_.setBounds(r);
}

// ------------------------------------------------------------------------------------------------ Section

Section::Section(const juce::String& title) {
    setTitle(title);
    setFocusContainerType(FocusContainerType::focusContainer);
    setWantsKeyboardFocus(false);
}

void Section::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(1.f);
    g.setColour(kPanel);
    g.fillRoundedRectangle(r, 6.f);
    g.setColour(kAccent.withAlpha(0.6f));
    g.drawRoundedRectangle(r, 6.f, 1.f);
    g.setColour(kAccent);
    g.setFont(uiFont(16.f, juce::Font::bold));
    g.drawText(getTitle(), getLocalBounds().reduced(10, 4).removeFromTop(20), juce::Justification::centredLeft);
}

std::unique_ptr<juce::AccessibilityHandler> Section::createAccessibilityHandler() {
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group);
}

// ------------------------------------------------------------------------------------------------ PatchListBox

void PatchListBox::focusSelectedRow() {
#if JUCE_WINDOWS
    if (auto* handler = getAccessibilityHandler())
        if (auto* table = handler->getTableInterface())
            if (const auto* row = table->getRowHandler(getSelectedRow()))
                const_cast<juce::AccessibilityHandler*>(row)->grabFocus();
#endif
}

void PatchListBox::focusGained(FocusChangeType cause) {
    juce::ListBox::focusGained(cause);
#if JUCE_WINDOWS
    // after JUCE's own focus handling for the list container
    juce::Component::SafePointer<PatchListBox> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe != nullptr && safe->hasKeyboardFocus(true)) safe->focusSelectedRow();
    });
#endif
}

bool PatchListBox::keyPressed(const juce::KeyPress& key) {
    const auto mods = key.getModifiers();
    // Ctrl+Page Up / Down (patch) and Ctrl+Shift+Page Up / Down (part) belong to the editor, not to list paging.
    if (mods.isCommandDown() &&
        (key.isKeyCode(juce::KeyPress::pageUpKey) || key.isKeyCode(juce::KeyPress::pageDownKey)))
        return false;

    const auto c = key.getTextCharacter();
    if (rowName && c >= ' ' && !mods.isCommandDown() && !mods.isAltDown() && !mods.isCtrlDown() &&
        getListBoxModel() != nullptr) {
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastKeyTime_ > 1000) prefix_.clear();
        lastKeyTime_ = now;
        prefix_ += juce::String::charToString(c);
        const int rows = getListBoxModel()->getNumRows();
        if (rows == 0) return true;
        const int sel = juce::jmax(0, getSelectedRow());
        // A single letter moves on to the next match; a longer prefix may stay on the current row.
        const int startOffset = prefix_.length() == 1 ? 1 : 0;
        for (int i = 0; i < rows; ++i) {
            const int r = (sel + startOffset + i) % rows;
            if (rowName(r).startsWithIgnoreCase(prefix_)) {
                selectRow(r);
                scrollToEnsureRowIsOnscreen(r);
                return true;
            }
        }
        return true;  // no match: keep the selection (the screen reader stays on the current row)
    }
    return juce::ListBox::keyPressed(key);
}

// ------------------------------------------------------------------------------------------------ HS2Editor

HS2Editor::HS2Editor(HS2Processor& p) : AudioProcessorEditor(&p), proc_(p) {
    setTitle("HS2 Port");
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    setWantsKeyboardFocus(true);
    int order = 1;

    // ---- Part
    addAndMakeVisible(partSection_);
    partSection_.setExplicitFocusOrder(order++);
    partSection_.addAndMakeVisible(partBox_);
    partBox_.setTitle("Part");
    setControlHelp(partBox_, "Choose the part to edit. Each part plays its corresponding MIDI channel.");
    partBox_.setExplicitFocusOrder(1);
    partBox_.onChange = [this] {
        const int id = partBox_.getSelectedId();
        if (id >= 1 && id <= kNumParts && id - 1 != part()) selectPart(id - 1);
    };
    setupLabel(partLabel_, "Part", partBox_);
    partSection_.addAndMakeVisible(clearPart_);
    clearPart_.setExplicitFocusOrder(2);
    setControlHelp(clearPart_, "Empties the selected part.");
    clearPart_.onClick = [this] {
        if (proc_.partInfo(part()).empty) {
            showError("Part " + juce::String(part() + 1) + " is already empty.");
            return;
        }
        proc_.requestClearPart(part());
    };
    partSection_.addAndMakeVisible(pageBox_);
    pageBox_.setTitle("Show");
    setControlHelp(pageBox_, "Choose an editing page. Ctrl+Tab and Ctrl+Shift+Tab cycle the pages.");
    pageBox_.setExplicitFocusOrder(3);
    pageBox_.addItemList({"Patch and mixer", "Part settings", "Arpeggiator", "Microtuning", "Patch parameters", "Elements",
                          "Patch send effects", "Global effects", "Global settings", "Patch information"},
                         1);
    pageBox_.onChange = [this] {
        const int id = pageBox_.getSelectedId();
        if (id >= 1 && id - 1 != page_) setPage(id - 1, false);
    };
    setupLabel(pageLabel_, "Show", pageBox_);
    setControlHelp(currentSound_, "Up and Down change the current sound. Page Up and Page Down jump ten. "
                   "Home and End go to the first and last. Enter focuses the patch browser.");
    currentSound_.onClick = [this] { search_.grabKeyboardFocus(); };
    currentSound_.navigate = [this](const juce::KeyPress& key) {
        using Key = juce::KeyPress;
        if (key.getModifiers().isAnyModifierKeyDown()) return false;
        if (key.isKeyCode(Key::returnKey)) { currentSound_.onClick(); return true; }
        if (key.isKeyCode(Key::upKey)) stepPatch(-1);
        else if (key.isKeyCode(Key::downKey)) stepPatch(1);
        else if (key.isKeyCode(Key::pageUpKey)) stepPatch(-10);
        else if (key.isKeyCode(Key::pageDownKey)) stepPatch(10);
        else if (key.isKeyCode(Key::homeKey)) stepPatch(-int(filtered_.size()));
        else if (key.isKeyCode(Key::endKey)) stepPatch(int(filtered_.size()));
        else return false;
        return true;
    };

    // ---- Patch browser
    addAndMakeVisible(browser_);
    browser_.setExplicitFocusOrder(order++);
    browser_.addAndMakeVisible(currentSound_);
    currentSound_.setExplicitFocusOrder(3);
    browser_.addAndMakeVisible(search_);
    search_.setTitle("Search patches");
    setControlHelp(search_, "Type words from a patch, combi or bank name. Enter moves to the patch list.");
    search_.setExplicitFocusOrder(1);
    search_.onTextChange = [this] { rebuildList(); };
    search_.onReturnKey = [this] {
        // Always move to the list: its name says how many patches match ("Patches, 3 shown" / "no patches match").
        list_.grabKeyboardFocus();
        if (!filtered_.empty() && list_.getSelectedRow() < 0) {
            suppressSelectionLoad_ = true;
            list_.selectRow(0);
            suppressSelectionLoad_ = false;
        }
    };
    setupLabel(searchLabel_, "Search", search_);

    browser_.addAndMakeVisible(bankBox_);
    bankBox_.setTitle("Bank filter");
    setControlHelp(bankBox_, "Show the patches of one bank, or of all banks. The combi banks hold combis.");
    bankBox_.setExplicitFocusOrder(2);
    bankBox_.onChange = [this] { rebuildList(); };
    setupLabel(bankLabel_, "Bank", bankBox_);

    browser_.addAndMakeVisible(list_);
    list_.setModel(this);
    list_.setRowHeight(24);
    list_.setTitle("Patches");
    setControlHelp(list_, "Type letters to jump to a patch. Enter loads the selected patch into the selected part; "
                         "a combi loads into all parts.");
    list_.setExplicitFocusOrder(4);
    list_.setColour(juce::ListBox::backgroundColourId, kBackground);
    list_.rowName = [this](int row) {
        return (library_ != nullptr && row >= 0 && row < int(filtered_.size()))
                   ? fromLatin1(library_->patches[size_t(filtered_[size_t(row)])].name)
                   : juce::String();
    };

    for (auto* b : {&loadButton_, &prevButton_, &nextButton_}) browser_.addAndMakeVisible(*b);
    loadButton_.setExplicitFocusOrder(5);
    prevButton_.setExplicitFocusOrder(6);
    nextButton_.setExplicitFocusOrder(7);
    setControlHelp(loadButton_, "Loads the patch selected in the list into the selected part, or the selected combi.");
    setControlHelp(prevButton_, "Loads the previous patch in the list.");
    setControlHelp(nextButton_, "Loads the next patch in the list.");
    loadButton_.onClick = [this] {
        if (list_.getSelectedRow() >= 0) loadRow(list_.getSelectedRow(), true);
        else showError("No patch is selected in the list.");
    };
    prevButton_.onClick = [this] { stepPatch(-1); };
    nextButton_.onClick = [this] { stepPatch(1); };

    autoLoad_.setButtonText("Load on selection");
    browser_.addAndMakeVisible(autoLoad_);
    autoLoad_.setExplicitFocusOrder(8);
    autoLoad_.setToggleState(true, juce::dontSendNotification);
    setControlHelp(autoLoad_, "When on, moving through the patch list loads each patch so you can play it at once. "
                             "Moving onto a combi loads the whole combi.");

    // ---- Current patch
    addAndMakeVisible(current_);
    current_.setExplicitFocusOrder(order++);
    current_.addAndMakeVisible(info_);
    setupReadOnly(info_, "Patch information",
                  "The selected part's patch: name, bank, description, controller notes, Hyperknob names, and the "
                  "loaded combi.",
                  true);
    info_.setExplicitFocusOrder(1);

    // ---- Hyperknobs of the selected part
    addAndMakeVisible(knobs_);
    knobs_.setExplicitFocusOrder(order++);
    for (int k = 0; k < kNumKnobs; ++k) {
        auto& s = knobSliders_[size_t(k)];
        setupSlider(s, k + 1, kSliderKeys);
        knobs_.addAndMakeVisible(s);
        setupLabel(knobLabels_[size_t(k)], "Hyperknob " + juce::String(k + 1), s);
    }

    // ---- Part mixer
    addAndMakeVisible(mixer_);
    mixer_.setExplicitFocusOrder(order++);
    mixer_.addAndMakeVisible(mute_);
    mute_.setExplicitFocusOrder(1);
    setControlHelp(mute_, "Silences the selected part.");
    setupSlider(level_, 2, "Part level. " + juce::String(kSliderKeys));
    setupSlider(pan_, 3, "Stereo position. " + juce::String(kSliderKeys));
    level_.setTitle("Level");
    pan_.setTitle("Pan");
    mixer_.addAndMakeVisible(level_);
    mixer_.addAndMakeVisible(pan_);
    setupLabel(levelLabel_, "Level", level_);
    setupLabel(panLabel_, "Pan", pan_);
    for (int k = 0; k < kNumSends; ++k) {
        auto& s = sends_[size_t(k)];
        setupSlider(s, 4 + k, "Level sent to effect " + juce::String(k + 1) + ". " + kSliderKeys);
        s.setTitle("Effect send " + juce::String(k + 1));
        mixer_.addAndMakeVisible(s);
        setupLabel(sendLabels_[size_t(k)], "Send " + juce::String(k + 1), s);
    }

    // ---- Pages: part settings, arpeggiator, microtuning (the selected part), global effects
    auto makePage = [&](Section& sec, ExtPage page, std::vector<std::unique_ptr<ParamRow>>& rows, std::vector<int>& es) {
        addChildComponent(sec);
        sec.setExplicitFocusOrder(order++);
        int o = 1;
        for (int e = 0; e < kExtPerPart; ++e)
            if (kExtDefs[e].page == page) {
                auto row = std::make_unique<ParamRow>(proc_, kExtDefs[e].name, juce::String(kExtDefs[e].name) + ".", o++);
                sec.addAndMakeVisible(*row);
                rows.push_back(std::move(row));
                es.push_back(e);
            }
        return o;
    };
    const int linkOrder = makePage(settingsPage_, ExtPage::settings, settingsRows_, settingsE_);
    linkRow_ = std::make_unique<ParamRow>(proc_, "Link", "When on, this part also plays the notes of the part above it.",
                                          linkOrder);
    settingsPage_.addChildComponent(*linkRow_);
    keepRow_ = std::make_unique<ParamRow>(proc_, "Keep Settings",
                                          "When on, the part keeps these settings when another patch loads into it.",
                                          linkOrder + 1);
    settingsPage_.addAndMakeVisible(*keepRow_);
    trimRow_ = std::make_unique<ParamRow>(proc_, "Level Trim", "The patch's level trim for this part.", linkOrder + 2);
    settingsPage_.addAndMakeVisible(*trimRow_);
    makePage(arpPage_, ExtPage::arp, arpRows_, arpE_);
    makePage(tuningPage_, ExtPage::tuning, tuningRows_, tuningE_);
    addChildComponent(gfxPage_);
    gfxPage_.setExplicitFocusOrder(order++);
    gfxPage_.addAndMakeVisible(gfxSlot_);
    gfxSlot_.setTitle("Effect slot");
    setControlHelp(gfxSlot_, "Which of the four global send effects the controls below work on. A combi sets them.");
    gfxSlot_.setExplicitFocusOrder(1);
    for (int k = 0; k < kGfxSlots; ++k) gfxSlot_.addItem("Effect " + juce::String(k + 1), k + 1);
    gfxSlot_.onChange = [this] {
        const int id = gfxSlot_.getSelectedId();
        if (id >= 1 && id <= kGfxSlots) {
            proc_.selectedEffectSlot = id - 1;
            bindPages();
            resized();
        }
    };
    setupLabel(gfxSlotLabel_, "Effect slot", gfxSlot_);
    gfxPage_.addAndMakeVisible(gfxEffect_);
    gfxEffect_.setTitle("Effect");
    setControlHelp(gfxEffect_, "The effect in this slot, from the effect presets in the content's Effects folder (as the "
                              "original's effect browser), by folder: Up and Down arrows load each one at once so you "
                              "can hear it; No effect empties the slot. Saved with the project.");
    gfxEffect_.setExplicitFocusOrder(2);
    gfxEffect_.onChange = [this] {
        const int id = gfxEffect_.getSelectedId();
        const int slot = juce::jlimit(0, kGfxSlots - 1, proc_.selectedEffectSlot.load());
        if (id == 2) proc_.requestGlobalFx(slot, "none");
        else if (id >= 3 && id - 3 < int(effectPaths_.size())) proc_.requestGlobalFx(slot, effectPaths_[size_t(id - 3)]);
    };
    setupLabel(gfxEffectLabel_, "Effect", gfxEffect_);
    {
        int o = 3;
        // Mute, Level, Pan, Aux Output, the effect's knobs (12: no effect has more), then Send 1 (effects 2..4)
        for (int j : {0, 18, 19, 17, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) {
            auto row = std::make_unique<ParamRow>(proc_, gfxDef(j).name, juce::String(gfxDef(j).name) + ".", o++);
            gfxPage_.addAndMakeVisible(*row);
            gfxRows_.push_back(std::move(row));
            gfxJ_.push_back(j);
        }
        gfxSend1Row_ = std::make_unique<ParamRow>(proc_, "Send to effect 1", "Level this effect sends to effect 1.", o++);
        gfxPage_.addChildComponent(*gfxSend1Row_);
    }

    // ---- The patch editor of the selected part: patch parameters, elements, patch send effects
    auto makeRows = [](Section& sec, std::vector<std::unique_ptr<ValueRow>>& rows, int count, int firstOrder) {
        for (int k = 0; k < count; ++k) {
            rows.push_back(std::make_unique<ValueRow>(firstOrder + k));
            sec.addChildComponent(*rows.back());
        }
    };
    auto makeHead = [](Section& sec, juce::ComboBox& box, juce::Label& label, const juce::String& title,
                       const juce::String& description, int o) {
        sec.addAndMakeVisible(box);
        box.setTitle(title);
        setControlHelp(box, description);
        box.setExplicitFocusOrder(o);
        setupLabel(label, title, box);
    };
    addChildComponent(patchPage_);
    patchPage_.setExplicitFocusOrder(order++);
    makeHead(patchPage_, patchGroupBox_, patchGroupLabel_, "Group",
             "Which of the patch's parameters the controls below show: Patch, Piano, Patch LFOs, Knob Ranges, GM "
             "Controls, Scenes, Arp Phrase, Arp Steps. Changes apply at once and are saved with the project; loading "
             "another patch into the part starts from that patch's own values.",
             1);
    patchGroupBox_.onChange = [this] {
        patchGroup_ = juce::jmax(0, patchGroupBox_.getSelectedItemIndex());
        refreshEditPages(true);
    };
    makeRows(patchPage_, patchRows_, kMaxGroupRows, 2);
    addChildComponent(elemPage_);
    elemPage_.setExplicitFocusOrder(order++);
    makeHead(elemPage_, elemBox_, elemLabel_, "Element",
             "The element of the part's patch the controls below work on: its number, type and name (an insert "
             "effect: its effect).",
             1);
    elemBox_.onChange = [this] {
        elemIndex_ = juce::jmax(0, elemBox_.getSelectedItemIndex());
        elemGroup_ = 0;
        refreshEditPages(true);
    };
    makeHead(elemPage_, elemGroupBox_, elemGroupLabel_, "Group",
             "Which of the element's parameters the controls below show: Main, the element type's own, Key and "
             "Velocity, Voice, Pitch, Glide, LFO 3, envelopes, Waveform, Filter, Output, Quick Knobs, and the 16 "
             "modulation rows (each named by its source and destination). Changes apply at once and are saved with "
             "the project.",
             2);
    elemGroupBox_.onChange = [this] {
        elemGroup_ = juce::jmax(0, elemGroupBox_.getSelectedItemIndex());
        refreshEditPages(true);
    };
    makeRows(elemPage_, elemRows_, kMaxGroupRows, 3);
    addChildComponent(sendsPage_);
    sendsPage_.setExplicitFocusOrder(order++);
    makeHead(sendsPage_, sendSlotBox_, sendSlotLabel_, "Send effect",
             "Which of the patch's own four send effects the controls below work on (the patch sets the effect). "
             "Changes apply at once and are saved with the project.",
             1);
    sendSlotBox_.onChange = [this] {
        sendSlot_ = juce::jlimit(0, kSendFxSlots - 1, sendSlotBox_.getSelectedItemIndex());
        refreshEditPages(true);
    };
    makeRows(sendsPage_, sendRows_, kSendFxParams, 2);
    ctx_.value = [this](int elem, int idx) {
        if (const auto it = editsSeen_.find({elem, idx}); it != editsSeen_.end()) return it->second.value;
        return snap_.get(elem, idx);
    };
    ctx_.elemType = [this](int elem) {
        return elem >= 1 && elem <= int(snap_.types.size()) ? snap_.types[size_t(elem - 1)] : -1;
    };
    ctx_.sendType = [this](int slot) { return slot >= 0 && slot < kSendFxSlots ? snap_.sendTypes[size_t(slot)] : 128; };

    // ---- Global settings
    addChildComponent(globalPage_);
    globalPage_.setExplicitFocusOrder(order++);
    {
        auto pct = [](float v) { return juce::String(juce::roundToInt(v * 100.f)) + " %"; };
        auto step100 = [](float v, int d) { return juce::jlimit(0.f, 1.f, float(juce::roundToInt(v * 100.f) + d) / 100.f); };
        auto signedText = [](int n, const char* unit) { return (n > 0 ? "+" : "") + juce::String(n) + unit; };
        std::vector<ValueRow::Binding> bs(6);
        bs[0].title = "Hype";
        bs[0].description = "The original's Hype: the drive of the master tone enhancer and the master level, for all "
                            "parts. Default 33 %.";
        bs[0].get = [this] { return proc_.hype(); };
        bs[0].set = [this](float v) { proc_.setHype(v); };
        bs[0].text = pct;
        bs[0].step = step100;
        bs[1].title = "Velocity sensitivity";
        bs[1].description = "How strongly note velocity acts, for all parts. 50 % is linear (the default); lower is "
                            "softer, higher steeper.";
        bs[1].get = [this] { return proc_.velSens(); };
        bs[1].set = [this](float v) { proc_.setVelSens(v); };
        bs[1].text = [pct](float v) { return pct(v) + (juce::roundToInt(v * 100.f) == 50 ? ", linear" : ""); };
        bs[1].step = step100;
        bs[2] = bs[1];
        bs[2].title = "Aftertouch sensitivity";
        bs[2].description = "How strongly aftertouch acts, for all parts. 50 % is linear (the default).";
        bs[2].get = [this] { return proc_.touchSens(); };
        bs[2].set = [this](float v) { proc_.setTouchSens(v); };
        bs[3].title = "Master tune";
        bs[3].description = "Transposes all parts, -12 to +12 semitones.";
        bs[3].get = [this] { return float(proc_.tuneSemis() + 12) / 24.f; };
        bs[3].set = [this](float v) { proc_.setTuneSemis(juce::roundToInt(v * 24.f) - 12); };
        bs[3].text = [signedText](float v) { return signedText(juce::roundToInt(v * 24.f) - 12, " semitones"); };
        bs[3].step = [](float v, int d) { return juce::jlimit(0.f, 1.f, float(juce::roundToInt(v * 24.f) + d) / 24.f); };
        bs[4].title = "Master fine tune";
        bs[4].description = "Tunes all parts, -50 to +50 cents.";
        bs[4].get = [this] { return float(proc_.tuneCents() + 50) / 100.f; };
        bs[4].set = [this](float v) { proc_.setTuneCents(juce::roundToInt(v * 100.f) - 50); };
        bs[4].text = [signedText](float v) { return signedText(juce::roundToInt(v * 100.f) - 50, " cents"); };
        bs[4].step = step100;
        bs[5].title = "Preload samples";
        bs[5].description = "When on, a patch's samples are all decoded while it loads, so the first notes play without "
                            "decoding them. Uses more memory. Applies to patches loaded after the change.";
        bs[5].get = [this] { return proc_.preloadSamples() ? 0.75f : 0.25f; };
        bs[5].set = [this](float v) { proc_.setPreloadSamples(v > 0.5f); };
        bs[5].text = [](float v) { return v > 0.5f ? juce::String("on") : juce::String("off"); };
        bs[5].choices = {{0.25f, "off"}, {0.75f, "on"}};
        const float defaults[5] = {0.33f, 0.5f, 0.5f, 0.5f, 0.5f};
        for (size_t k = 0; k < bs.size(); ++k) {
            if (k < 5) {
                const float d = defaults[k];
                auto set = bs[k].set;
                bs[k].reset = [set, d] { set(d); };
            }
            globalRows_.push_back(std::make_unique<ValueRow>(int(k) + 1));
            globalPage_.addChildComponent(*globalRows_.back());
            globalRows_.back()->bind(bs[k]);
        }
    }

    // ---- Master
    addAndMakeVisible(master_);
    master_.setExplicitFocusOrder(order++);
    setupSlider(masterVolume_, 1, "Volume of all parts. " + juce::String(kSliderKeys));
    masterVolume_.setTitle("Master volume");
    master_.addAndMakeVisible(masterVolume_);
    masterAttach_ = std::make_unique<juce::SliderParameterAttachment>(proc_.exposed(0), masterVolume_);
    masterVolume_.defaultValue = [this] { return double(proc_.loadedValue(0)); };
    setupLabel(masterLabel_, "Master volume", masterVolume_);

    // ---- Settings
    addAndMakeVisible(settings_);
    settings_.setExplicitFocusOrder(order++);
    settings_.addAndMakeVisible(contentPath_);
    setupReadOnly(contentPath_, "Content folder", "The Hypersonic 2 Content folder the patches and samples load from.",
                  false);
    contentPath_.setExplicitFocusOrder(1);
    setupLabel(contentLabel_, "Content folder", contentPath_);
    settings_.addAndMakeVisible(chooseContent_);
    chooseContent_.setExplicitFocusOrder(2);
    setControlHelp(chooseContent_, "Opens a folder chooser for the Hypersonic 2 Content folder.");
    chooseContent_.onClick = [this] { chooseContentFolder(); };
    settings_.addAndMakeVisible(memoryMode_);
    memoryMode_.setTitle("Piano memory mode");
    setControlHelp(memoryMode_, "Sample set for the piano patches, for every part.");
    memoryMode_.addItem("ECO", 1);
    memoryMode_.addItem("MID", 2);
    memoryMode_.addItem("XXL", 3);
    memoryMode_.setSelectedId(proc_.memoryMode() + 1, juce::dontSendNotification);
    memoryMode_.setExplicitFocusOrder(3);
    memoryMode_.onChange = [this] { proc_.setMemoryMode(memoryMode_.getSelectedId() - 1); };
    setupLabel(memoryLabel_, "Piano memory", memoryMode_);
    midiPrograms_.setButtonText("MIDI program changes");
    settings_.addAndMakeVisible(midiPrograms_);
    midiPrograms_.setExplicitFocusOrder(4);
    setControlHelp(midiPrograms_, "When on, MIDI bank select (controller 32 = bank number) and program change load a "
                                 "patch into the part of that MIDI channel; banks 120 and up load combis.");
    midiPrograms_.setToggleState(proc_.midiProgramChanges(), juce::dontSendNotification);
    midiPrograms_.onClick = [this] { proc_.setMidiProgramChanges(midiPrograms_.getToggleState()); };
    ringOut_.setButtonText("Effect tails");
    settings_.addAndMakeVisible(ringOut_);
    ringOut_.setExplicitFocusOrder(5);
    setControlHelp(ringOut_, "When on, delay and reverb tails play out after the last note. The original Hypersonic 2 "
                            "cuts some of them off; turn this off to hear it exactly as the original.");
    ringOut_.setToggleState(proc_.ringOut(), juce::dontSendNotification);
    ringOut_.onClick = [this] { proc_.setRingOut(ringOut_.getToggleState()); };
    cutOnLoad_.setButtonText("Cut sound on patch change");
    settings_.addAndMakeVisible(cutOnLoad_);
    cutOnLoad_.setExplicitFocusOrder(6);
    setControlHelp(cutOnLoad_, "When on, loading a patch or combi stops the notes and effect tails of the part's "
                              "previous patch (with a short fade). When off, they sound on until they end.");
    cutOnLoad_.setToggleState(proc_.cutOnLoad(), juce::dontSendNotification);
    cutOnLoad_.onClick = [this] { proc_.setCutOnLoad(cutOnLoad_.getToggleState()); };
    settings_.addAndMakeVisible(installPresets_);
    installPresets_.setExplicitFocusOrder(7);
    setControlHelp(installPresets_, "Writes a preset file for every patch and combi in your content folder, so you can "
                                   "choose them in your DAW's preset browser and in Komplete Kontrol. Presets you saved "
                                   "yourself are kept.");
    installPresets_.onClick = [this] { confirmInstallPresets(); };
    settings_.addAndMakeVisible(allNotesOff_);
    allNotesOff_.setExplicitFocusOrder(8);
    setControlHelp(allNotesOff_, "Stops every sounding note on all 16 parts.");
    allNotesOff_.onClick = [this] {
        proc_.allNotesOff();
        if (view_ == View::instrument) status_.setText("All notes off.", juce::dontSendNotification);
    };
    settings_.addAndMakeVisible(rescan_);
    rescan_.setExplicitFocusOrder(9);
    setControlHelp(rescan_, "Reads the content folder's patch banks, combis, tunings and effect presets again, for "
                           "example after you added banks to it.");
    rescan_.onClick = [this] { proc_.rescanContent(); };
    addAndMakeVisible(preferencesBar_);
    preferencesBar_.setFocusContainerType(FocusContainerType::focusContainer);
    preferencesBar_.setExplicitFocusOrder(1);
    partSection_.addAndMakeVisible(preferences_);
    partSection_.addAndMakeVisible(help_);
    preferences_.setExplicitFocusOrder(5);
    setControlHelp(preferences_, "Open General preferences. Ctrl+comma also opens Preferences.");
    preferences_.onClick = [this] { showView(View::general); };
    help_.setExplicitFocusOrder(6);
    help_.onClick = [this] { showKeyboardHelp(); };
    if (proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
#if JUCE_WINDOWS || JUCE_MAC
        partSection_.addAndMakeVisible(hide_);
#if JUCE_MAC
        hide_.setButtonText("Hide to Menu Bar");
#endif
        hide_.setExplicitFocusOrder(7);
        hide_.onClick = [this] { hideToTray(); };
#endif
        settings_.addAndMakeVisible(updates_);
        updates_.setButtonText("Check for updates...");
        updates_.setExplicitFocusOrder(10);
        updates_.onClick = [this] { checkForUpdates(); };
        partSection_.addAndMakeVisible(keyboard_);
        keyboard_.setExplicitFocusOrder(8);
        keyboard_.setClickingTogglesState(false);
        keyboard_.onClick = [this] { toggleKeyboard(); };
        setControlHelp(keyboard_, "Play the selected part from the computer keyboard. Ctrl+K toggles it. "
                       "A through apostrophe play the white keys, W E T Y U O P the sharps. "
                       "Z and X change octave; C and V change velocity.");
        addChildComponent(keyboardPanel_);
        keyboardPanel_.setExplicitFocusOrder(100);
        auto setup = [this](KeySlider& slider, juce::Label& label, const char* name, int lo, int hi, int order) {
            keyboardPanel_.addAndMakeVisible(slider);
            setupSlider(slider, order, "Enter edits the value.");
            slider.setTitle(name);
            slider.setRange(lo, hi, 1);
            setupLabel(label, name, slider);
        };
        setup(octave_, octaveLabel_, "Octave", lowestTypingOctave, highestTypingOctave, 1);
        setup(velocity_, velocityLabel_, "Velocity", lowestTypingVelocity, highestTypingVelocity, 2);
        octave_.onValueChange = [this] { typing_.setOctave(int(octave_.getValue())); };
        velocity_.onValueChange = [this] { typing_.setVelocity(int(velocity_.getValue())); };
    }
    for (auto* b : {&generalTab_, &audioTab_, &back_}) preferencesBar_.addAndMakeVisible(*b);
    generalTab_.setExplicitFocusOrder(1);
    audioTab_.setExplicitFocusOrder(2);
    back_.setExplicitFocusOrder(3);
    setControlHelp(generalTab_, "Content, playback options, and preset export.");
    setControlHelp(audioTab_, "Audio driver, output, buffer, sample rate, and MIDI inputs.");
    setControlHelp(back_, "Return to the instrument. Escape also returns.");
    setControlHelp(updates_, "Check for a newer version on GitHub. No files are installed.");
    auto setupPresetFolder = [this](PresetFolderRow& row, const juce::String& name, PresetDestination format, int order) {
        settings_.addAndMakeVisible(row.path);
        setupReadOnly(row.path, name + " folder", "The exact destination for this format.", false);
        row.path.setExplicitFocusOrder(order);
        setupLabel(row.label, name + " folder", row.path);
        settings_.addAndMakeVisible(row.browse);
        row.browse.setButtonText("Browse...");
        row.browse.setTitle("Browse " + name + " folder");
        row.browse.setExplicitFocusOrder(order + 1);
        row.browse.onClick = [this, format] { choosePresetFolder(format); };
        settings_.addAndMakeVisible(row.reset);
        row.reset.setTitle("Use default " + name + " folder");
        row.reset.setExplicitFocusOrder(order + 2);
        row.reset.onClick = [this, &row, format] {
            proc_.setPresetFolderOverride(format, {});
            refreshPresetFolders();
            row.browse.grabKeyboardFocus();
        };
    };
    setupPresetFolder(vst3Folder_, "VST3 presets", PresetDestination::vst3, 7);
    setupPresetFolder(nksFolder_, "Komplete Kontrol presets", PresetDestination::nks, 10);
    installPresets_.setExplicitFocusOrder(13);
    allNotesOff_.setExplicitFocusOrder(14);
    rescan_.setExplicitFocusOrder(15);
    updates_.setExplicitFocusOrder(16);
    generalTab_.setRadioGroupId(1);
    audioTab_.setRadioGroupId(1);
    generalTab_.setClickingTogglesState(true);
    audioTab_.setClickingTogglesState(true);
    generalTab_.onClick = [this] { showView(View::general); };
    audioTab_.onClick = [this] { showView(View::audio); };
    audioTab_.setVisible(standaloneAudioAvailable());
    back_.onClick = [this] { showView(View::instrument); };
    addAndMakeVisible(helpPane_);
    helpPane_.addAndMakeVisible(helpText_);
    helpPane_.addAndMakeVisible(closeHelp_);
    setupReadOnly(helpText_, "Help", {}, true);
    helpText_.setExplicitFocusOrder(1);
    closeHelp_.setExplicitFocusOrder(2);
    closeHelp_.onClick = [this] {
        auto focus = returnFocus_;
        showView(helpReturn_);
        if (focus && focus->isShowing()) focus->grabKeyboardFocus();
    };
    helpText_.onEscapeKey = [this] { closeHelp_.onClick(); };

    for (auto [control, key] : std::initializer_list<std::pair<juce::Component*, char>>{
             {&currentSound_, 'w'}, {&clearPart_, 'c'}, {&bankBox_, 'b'}, {&loadButton_, 'u'},
             {&autoLoad_, 'o'}, {&mute_, 'f'}, {&masterVolume_, 'm'},
             {&generalTab_, 'g'}, {&audioTab_, 'a'},
             {&chooseContent_, 'c'}, {&memoryMode_, 'p'}, {&midiPrograms_, 'm'}, {&ringOut_, 'e'},
             {&cutOnLoad_, 's'}, {&installPresets_, 'i'}, {&rescan_, 'r'}})
        setControlMnemonic(*control, key);

    for (auto [control, shortcut] : std::initializer_list<std::pair<juce::Component*, const char*>>{
             {&partBox_, "Ctrl+P"}, {&pageBox_, "Ctrl+Tab"}, {&search_, "Ctrl+F"}, {&list_, "Ctrl+L"},
             {&prevButton_, "Ctrl+Page Up"}, {&nextButton_, "Ctrl+Page Down"}, {&info_, "Ctrl+I"},
             {&preferences_, "Ctrl+,"}, {&help_, "F1"}, {&keyboard_, "Ctrl+K"}, {&hide_, "Ctrl+H"},
             {&updates_, "Shift+F1"},
             {&allNotesOff_, "F9"}, {&back_, "Escape"}, {&closeHelp_, "Escape"}})
        setControlAccelerator(*control, shortcut);

    // ---- Status
    addAndMakeVisible(statusSection_);
    statusSection_.setExplicitFocusOrder(order++);
    statusSection_.addAndMakeVisible(status_);
    setupReadOnly(status_, "Status", "Loading progress, errors and unsupported features of the loaded patches.", true);
    setControlMnemonic(status_, 's');
    contentPath_.onEscapeKey = [this] { showView(View::instrument); };

    for (auto* t : std::initializer_list<juce::TextEditor*>{&search_, &info_, &contentPath_, &status_, &helpText_}) {
        t->setColour(juce::TextEditor::backgroundColourId, kBackground);
        t->setColour(juce::TextEditor::textColourId, kText);
        t->setFont(uiFont(15.f));
    }

    setResizable(true, true);
    setResizeLimits(900, 610, 2000, 1600);
    setSize(1080, 690);
    refreshPartBox();
    selectPart(part());
    refreshFromProcessor();
    showView(View::instrument);
    startTimerHz(10);
}

HS2Editor::~HS2Editor() {
    typing_.releaseAll([this](int c, int p, int v) { sendTypingNote(c, p, v); });
    stopTimer();
    list_.setModel(nullptr);
}

void HS2Editor::visibilityChanged() {
    if (isShowing() && !focusedOnce_) {
        focusedOnce_ = true;
        juce::Component::SafePointer<HS2Editor> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) {
                safe->configureStandaloneWindow();
                if (safe->view_ == View::instrument) safe->partBox_.grabKeyboardFocus();
            }
        });
    }
}

void HS2Editor::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void HS2Editor::focusGained(FocusChangeType) {
    // The editor itself is not a useful stop (Tab cannot move on from it): hand focus to the search box.
    juce::Component::SafePointer<HS2Editor> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe != nullptr && safe->hasKeyboardFocus(false))
            (safe->view_ == View::instrument ? static_cast<juce::Component&>(safe->partBox_)
                                             : static_cast<juce::Component&>(safe->back_)).grabKeyboardFocus();
    });
}

void HS2Editor::focusOfChildComponentChanged(FocusChangeType) {
    // Do not hide a value editor while it is taking focus; suspend notes instead.
    refreshKeyboard();
}

void HS2Editor::resized() {
    auto all = getLocalBounds().reduced(8);
    const int gap = 8;
    auto top = all.removeFromTop(98);
    partSection_.setBounds(top);
    preferencesBar_.setBounds(top);
    auto bar = preferencesBar_.getLocalBounds().reduced(10).withHeight(32);
    generalTab_.setBounds(bar.removeFromLeft(145).reduced(2));
    audioTab_.setBounds(bar.removeFromLeft(165).reduced(2));
    back_.setBounds(bar.removeFromRight(180).reduced(2));
    all.removeFromTop(gap);
    if (keyboardPanel_.isVisible()) {
        keyboardPanel_.setBounds(all.removeFromBottom(64));
        all.removeFromBottom(gap);
        auto strip = keyboardPanel_.content();
        octave_.setBounds(strip.removeFromLeft(strip.getWidth() / 2).withTrimmedLeft(80).reduced(4, 0));
        velocity_.setBounds(strip.withTrimmedLeft(90).reduced(4, 0));
    }
    helpPane_.setBounds(all);
    auto helpBounds = helpPane_.getLocalBounds();
    closeHelp_.setBounds(helpBounds.removeFromBottom(36).removeFromRight(120));
    helpText_.setBounds(helpBounds);
    if (view_ == View::audio && audioPanel_ != nullptr) audioPanel_->setBounds(all);
    if (view_ == View::general) settings_.setBounds(all.withTrimmedRight(juce::jmax(0, all.getWidth() - 850)));
    auto r = all.removeFromRight(juce::jmax(560, all.getWidth() * 58 / 100));
    all.removeFromRight(gap);
    browser_.setBounds(all);
    auto statusArea = r.removeFromBottom(78);
    r.removeFromBottom(gap);
    auto masterArea = r.removeFromBottom(64);
    r.removeFromBottom(gap);
    auto mixerArea = r.removeFromBottom(162);
    r.removeFromBottom(gap);
    auto knobsArea = r.removeFromBottom(132);
    r.removeFromBottom(gap);
    auto currentArea = r;
    knobs_.setBounds(knobsArea);
    mixer_.setBounds(mixerArea);
    // the other pages: the whole area of current patch, Hyperknobs and mixer
    const auto pageArea = currentArea.getUnion(mixerArea);
    current_.setBounds(page_ == pgInfo ? pageArea : currentArea);
    for (auto* sec : {&settingsPage_, &arpPage_, &tuningPage_, &gfxPage_, &patchPage_, &elemPage_, &sendsPage_, &globalPage_})
        sec->setBounds(pageArea);
    {
        std::vector<juce::Component*> rows;
        for (auto& r2 : settingsRows_) rows.push_back(r2.get());
        if (linkRow_->isVisible()) rows.push_back(linkRow_.get());
        rows.push_back(keepRow_.get());
        rows.push_back(trimRow_.get());
        layoutRows(settingsPage_, rows, {});
        rows.clear();
        for (auto& r2 : arpRows_) rows.push_back(r2.get());
        layoutRows(arpPage_, rows, {});
        rows.clear();
        for (auto& r2 : tuningRows_) rows.push_back(r2.get());
        layoutRows(tuningPage_, rows, {});
        rows.clear();
        for (auto& r2 : gfxRows_)
            if (r2->isVisible()) rows.push_back(r2.get());
        if (gfxSend1Row_->isVisible()) rows.push_back(gfxSend1Row_.get());
        layoutRows(gfxPage_, rows, {{&gfxSlot_, &gfxSlotLabel_}, {&gfxEffect_, &gfxEffectLabel_}});
        auto shown = [](const std::vector<std::unique_ptr<ValueRow>>& rs) {
            std::vector<juce::Component*> v;
            for (auto& r2 : rs)
                if (r2->isVisible()) v.push_back(r2.get());
            return v;
        };
        layoutRows(patchPage_, shown(patchRows_), {{&patchGroupBox_, &patchGroupLabel_}});
        layoutRows(elemPage_, shown(elemRows_), {{&elemBox_, &elemLabel_}, {&elemGroupBox_, &elemGroupLabel_}});
        layoutRows(sendsPage_, shown(sendRows_), {{&sendSlotBox_, &sendSlotLabel_}});
        layoutRows(globalPage_, shown(globalRows_), {});
    }
    master_.setBounds(masterArea);
    statusSection_.setBounds(statusArea);

    {
        auto c = partSection_.content().withHeight(28);
        c.removeFromLeft(60);
        partBox_.setBounds(c.removeFromLeft(300));
        c.removeFromLeft(10);
        clearPart_.setBounds(c.removeFromLeft(140));
        help_.setBounds(c.removeFromRight(128).reduced(2, 0));
        if (hide_.isVisible()) hide_.setBounds(c.removeFromRight(145).reduced(2, 0));
        preferences_.setBounds(c.removeFromRight(112).reduced(2, 0));
        auto c2 = partSection_.content().withTrimmedTop(34).withHeight(28);
        c2.removeFromLeft(60);
        pageBox_.setBounds(c2.removeFromLeft(300));
        c2.removeFromLeft(10);
        if (keyboard_.isVisible()) keyboard_.setBounds(c2.removeFromRight(160));
    }
    {
        auto c = browser_.content();
        auto row = c.removeFromTop(28);
        row.removeFromLeft(52);
        search_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 17));
        row.removeFromLeft(60);
        bankBox_.setBounds(row);
        c.removeFromTop(6);
        currentSound_.setBounds(c.removeFromTop(28));
        c.removeFromTop(6);
        autoLoad_.setBounds(c.removeFromBottom(28));
        c.removeFromBottom(4);
        auto buttons = c.removeFromBottom(28);
        c.removeFromBottom(6);
        list_.setBounds(c);
        const int bw = (buttons.getWidth() - 12) / 3;
        loadButton_.setBounds(buttons.removeFromLeft(bw));
        buttons.removeFromLeft(6);
        prevButton_.setBounds(buttons.removeFromLeft(bw));
        buttons.removeFromLeft(6);
        nextButton_.setBounds(buttons);
    }
    info_.setBounds(current_.content());
    {
        auto c = knobs_.content();
        const int colW = c.getWidth() / 2;
        for (int k = 0; k < kNumKnobs; ++k) {
            const int col = k / 3, rowIdx = k % 3;
            juce::Rectangle<int> cell(c.getX() + col * colW, c.getY() + rowIdx * 32, colW - 10, 28);
            knobSliders_[size_t(k)].setBounds(cell.withTrimmedLeft(115));
        }
    }
    {
        auto c = mixer_.content();
        const int colW = c.getWidth() / 2;
        auto cell = [&](int col, int row) {
            return juce::Rectangle<int>(c.getX() + col * colW, c.getY() + row * 30, colW - 10, 28);
        };
        mute_.setBounds(cell(0, 0).withTrimmedLeft(8));
        level_.setBounds(cell(0, 1).withTrimmedLeft(70));
        pan_.setBounds(cell(0, 2).withTrimmedLeft(70));
        for (int k = 0; k < kNumSends; ++k) sends_[size_t(k)].setBounds(cell(1, k).withTrimmedLeft(70));
    }
    masterVolume_.setBounds(master_.content().withTrimmedLeft(130).withHeight(28));
    {
        auto c = settings_.content();
        auto row1 = c.removeFromTop(30);
        row1.removeFromLeft(120);
        chooseContent_.setBounds(row1.removeFromRight(200));
        row1.removeFromRight(6);
        contentPath_.setBounds(row1);
        c.removeFromTop(8);
        auto row2 = c.removeFromTop(30);
        row2.removeFromLeft(120);
        memoryMode_.setBounds(row2.removeFromLeft(320));
        c.removeFromTop(8);
        midiPrograms_.setBounds(c.removeFromTop(30).withTrimmedLeft(120));
        c.removeFromTop(6);
        ringOut_.setBounds(c.removeFromTop(30).withTrimmedLeft(120));
        c.removeFromTop(6);
        cutOnLoad_.setBounds(c.removeFromTop(30).withTrimmedLeft(120));
        c.removeFromTop(6);
        for (auto* folder : {&vst3Folder_, &nksFolder_}) {
            auto line = c.removeFromTop(30).withTrimmedLeft(210);
            folder->reset.setBounds(line.removeFromRight(105));
            line.removeFromRight(6);
            folder->browse.setBounds(line.removeFromRight(95));
            line.removeFromRight(6);
            folder->path.setBounds(line);
            c.removeFromTop(8);
        }
        auto row6 = c.removeFromTop(30).withTrimmedLeft(120);
        installPresets_.setBounds(row6.removeFromLeft(200));
        row6.removeFromLeft(6);
        allNotesOff_.setBounds(row6.removeFromLeft(140));
        row6.removeFromLeft(6);
        rescan_.setBounds(row6.removeFromLeft(200));
        c.removeFromTop(8);
        updates_.setBounds(c.removeFromTop(30).withTrimmedLeft(120).removeFromLeft(200));
    }
    status_.setBounds(statusSection_.content());
}

// ---- parts

void HS2Editor::refreshPartBox() {
    // Rebuilt only when a part's patch changed, so the screen reader is not disturbed otherwise.
    juce::StringArray texts;
    for (int p = 0; p < kNumParts; ++p) texts.add(proc_.partSummary(p));
    bool same = partBox_.getNumItems() == kNumParts;
    for (int p = 0; same && p < kNumParts; ++p) same = partBox_.getItemText(p) == texts[p];
    if (!same) {
        partBox_.clear(juce::dontSendNotification);
        for (int p = 0; p < kNumParts; ++p) partBox_.addItem(texts[p], p + 1);
    }
    if (partBox_.getSelectedId() != part() + 1) partBox_.setSelectedId(part() + 1, juce::dontSendNotification);
}

// Rows in two columns (row-major), after an optional first control (the effect slot) on its own line.
void HS2Editor::layoutRows(Section& s, const std::vector<juce::Component*>& rows,
                           const std::vector<std::pair<juce::Component*, juce::Label*>>& heads) {
    auto c = s.content();
    for (const auto& [ctl, label] : heads) {
        auto line = c.removeFromTop(28);
        if (label != nullptr) label->setBounds(line.removeFromLeft(110));
        ctl->setBounds(line.removeFromLeft(juce::jmin(420, line.getWidth())));
        c.removeFromTop(6);
    }
    const int colW = (c.getWidth() - 12) / 2, rowH = 28, gap = 4;
    for (size_t i = 0; i < rows.size(); ++i) {
        const int col = int(i % 2), row = int(i / 2);
        rows[i]->setBounds(c.getX() + col * (colW + 12), c.getY() + row * (rowH + gap), colW, rowH);
    }
}

void HS2Editor::fillEffectBox() {
    const int slot = juce::jlimit(0, kGfxSlots - 1, proc_.selectedEffectSlot.load());
    const auto lib = proc_.library();
    const auto choice = proc_.gfxChoice(slot);
    const auto text = proc_.gfxSlotText(slot);
    // the list changes with the library, the slot, and whether the slot holds a preset chosen here
    const auto sig = juce::String::toHexString(juce::pointer_sized_int(lib.get())) + "|" + juce::String(slot) + "|" +
                     (choice.isEmpty() ? "loaded:" + text : choice.startsWith("none") ? "none" : "preset:" + choice);
    if (sig == seenEffectSig_) return;
    seenEffectSig_ = sig;
    const bool focused = gfxEffect_.hasKeyboardFocus(true);
    const int was = gfxEffect_.getSelectedId();
    gfxEffect_.clear(juce::dontSendNotification);
    effectPaths_.clear();
    int selected = 0;
    if (choice.isEmpty()) {  // the combi's or patch's effect: shown, but not one to go back to (arrows skip it)
        gfxEffect_.addItem(text == "empty" ? juce::String("No effect, as loaded") : text + ", as loaded", 1);
        gfxEffect_.setItemEnabled(1, false);
        selected = 1;
    }
    gfxEffect_.addItem("No effect", 2);
    if (choice == "none") selected = 2;
    if (lib != nullptr)
        for (const auto& fx : lib->effects) {
            const juce::String path(fx.rel.generic_wstring().c_str());
            effectPaths_.push_back(path);
            const int id = 2 + int(effectPaths_.size());
            const auto folder = path.containsChar('/') ? path.upToFirstOccurrenceOf("/", false, false) : juce::String();
            const auto name = path.fromLastOccurrenceOf("/", false, false).upToLastOccurrenceOf(".", false, false);
            gfxEffect_.addItem(folder.isNotEmpty() ? folder + ": " + name : name, id);
            if (path == choice) selected = id;
        }
    if (selected == 0) {  // a preset chosen before that is no longer in the folder
        gfxEffect_.addItem(text + ", as loaded", 1);
        gfxEffect_.setItemEnabled(1, false);
        selected = 1;
    }
    // While the box has focus the user's own pick stays shown (its load may still be on the way).
    if (focused && was > 1 && gfxEffect_.indexOfItemId(was) >= 0) selected = was;
    gfxEffect_.setSelectedId(selected, juce::dontSendNotification);
}

void HS2Editor::setPage(int page, bool focusSelector) {
    page_ = juce::jlimit(0, int(kNumPages) - 1, page);
    proc_.selectedPage = page_;
    if (pageBox_.getSelectedId() != page_ + 1) pageBox_.setSelectedId(page_ + 1, juce::dontSendNotification);
    const bool instrument = view_ == View::instrument;
    current_.setVisible(instrument && (page_ == pgPatch || page_ == pgInfo));
    knobs_.setVisible(instrument && page_ == pgPatch);
    mixer_.setVisible(instrument && page_ == pgPatch);
    settingsPage_.setVisible(instrument && page_ == pgSettings);
    arpPage_.setVisible(instrument && page_ == pgArp);
    tuningPage_.setVisible(instrument && page_ == pgTuning);
    patchPage_.setVisible(instrument && page_ == pgPatchParams);
    elemPage_.setVisible(instrument && page_ == pgElements);
    sendsPage_.setVisible(instrument && page_ == pgSends);
    gfxPage_.setVisible(instrument && page_ == pgGfx);
    globalPage_.setVisible(instrument && page_ == pgGlobal);
    // The audio thread copies the part's values for the patch editor pages only while one is shown.
    const bool editPage = page_ == pgPatchParams || page_ == pgElements || page_ == pgSends;
    proc_.setEditPart(instrument && editPage ? part() : -1);
    if (instrument && editPage) refreshEditPages(true);
    resized();
    if (focusSelector) pageBox_.grabKeyboardFocus();
}

// The first control of the shown page (Ctrl+Tab goes there, so the screen reader reads the page and that control).
void HS2Editor::focusPage() {
    juce::Component* first = nullptr;
    switch (page_) {
        case pgInfo: first = &info_; break;
        case pgPatch: first = &knobSliders_.front(); break;
        case pgGfx: first = &gfxSlot_; break;
        case pgPatchParams: first = &patchGroupBox_; break;
        case pgElements: first = &elemBox_; break;
        case pgSends: first = &sendSlotBox_; break;
        case pgGlobal:
            for (auto* c : globalRows_.front()->getChildren())
                if (c->isVisible() && c->getWantsKeyboardFocus()) first = c;
            break;
        default: {
            auto& rows = page_ == pgSettings ? settingsRows_ : page_ == pgArp ? arpRows_ : tuningRows_;
            if (!rows.empty())
                for (auto* c : rows.front()->getChildren())
                    if (c->isVisible() && c->getWantsKeyboardFocus()) first = c;
            break;
        }
    }
    if (first != nullptr) first->grabKeyboardFocus();
}

void HS2Editor::bindPages() {
    const int p = part();
    const juce::String n = "Part " + juce::String(p + 1);
    settingsPage_.setTitle(n + " settings");
    arpPage_.setTitle(n + " arpeggiator");
    tuningPage_.setTitle(n + " microtuning");
    patchPage_.setTitle(n + " patch parameters");
    elemPage_.setTitle(n + " elements");
    sendsPage_.setTitle(n + " patch send effects");
    for (size_t r = 0; r < settingsRows_.size(); ++r) settingsRows_[r]->bind(extIndex(p, settingsE_[r]));
    for (size_t r = 0; r < arpRows_.size(); ++r) arpRows_[r]->bind(extIndex(p, arpE_[r]));
    for (size_t r = 0; r < tuningRows_.size(); ++r) tuningRows_[r]->bind(extIndex(p, tuningE_[r]));
    linkRow_->setVisible(p > 0);  // part 1 has no part above it
    linkRow_->bind(p > 0 ? linkIndex(p) : -1);
    keepRow_->bind(partExtraIndex(p, pxKeep));
    trimRow_->bind(partExtraIndex(p, pxTrim));
    const int slot = juce::jlimit(0, kGfxSlots - 1, proc_.selectedEffectSlot.load());
    if (gfxSlot_.getSelectedId() != slot + 1) gfxSlot_.setSelectedId(slot + 1, juce::dontSendNotification);
    for (size_t r = 0; r < gfxRows_.size(); ++r) gfxRows_[r]->bind(gfxIndex(slot, gfxJ_[r]));
    gfxSend1Row_->setVisible(slot > 0);
    gfxSend1Row_->bind(slot > 0 ? gfxIndex(slot, 20) : -1);
    for (int t = 0; t < 2; ++t) seenTuneBank_[size_t(t)] = -1;
    gfxKnobSeen_.assign(gfxRows_.size(), reinterpret_cast<const void*>(1));  // none yet: every knob row rebinds
    seenEffectSig_.clear();
    seenFxNames_.clear();
    refreshPages();
    for (auto* sec : {&settingsPage_, &arpPage_, &tuningPage_, &gfxPage_, &patchPage_, &elemPage_, &sendsPage_}) sec->repaint();
    // the patch editor pages start over on the new part (its values arrive from the audio thread)
    unbindEditRows();
    elemIndex_ = elemGroup_ = 0;
    if (page_ == pgPatchParams || page_ == pgElements || page_ == pgSends) {
        proc_.setEditPart(p);
        refreshEditPages(true);
    }
}

void HS2Editor::unbindEditRows() {
    for (auto* rows : {&patchRows_, &elemRows_, &sendRows_})
        for (auto& r : *rows) r->unbind();
    snapPart_ = snapSerial_ = -1;
}

void HS2Editor::setBoxItems(juce::ComboBox& box, const juce::StringArray& items, int selectedIndex) {
    bool same = box.getNumItems() == items.size();
    for (int k = 0; same && k < items.size(); ++k) same = box.getItemText(k) == items[k];
    if (!same) {
        box.clear(juce::dontSendNotification);
        for (int k = 0; k < items.size(); ++k) box.addItem(items[k], k + 1);
    }
    if (box.getSelectedItemIndex() != selectedIndex) box.setSelectedItemIndex(selectedIndex, juce::dontSendNotification);
}

void HS2Editor::refreshEditPages(bool rebindAll) {
    if (page_ != pgPatchParams && page_ != pgElements && page_ != pgSends) return;
    const int p = part();
    PartSnapshot fresh;
    if (!proc_.snapshot(p, fresh)) return;  // the audio thread has not copied this part yet: next time
    if (fresh.part != p) return;
    const bool newPatch = p != snapPart_ || fresh.serial != snapSerial_ || fresh.types != snap_.types;
    snap_ = std::move(fresh);
    editsSeen_ = proc_.edits(p);
    if (newPatch) {
        snapPart_ = p;
        snapSerial_ = snap_.serial;
        content_ = proc_.editorContent();
        rebindAll = true;
    }
    ctx_.library = library_.get();
    ctx_.content = content_.get();
    const auto info = proc_.partInfo(p);
    if (page_ == pgPatchParams) {
        std::vector<EditGroup> groups;
        juce::StringArray titles;
        if (info.empty) titles.add("None, the part is empty");
        else
            for (auto& g : (groups = patchGroups(ctx_))) titles.add(g.title);
        patchGroup_ = juce::jlimit(0, titles.size() - 1, patchGroup_);
        setBoxItems(patchGroupBox_, titles, patchGroup_);
        bindEditRows(patchRows_, 0, groups.empty() ? std::vector<int>() : groups[size_t(patchGroup_)].idx, rebindAll);
    } else if (page_ == pgElements) {
        juce::StringArray titles;
        const int n = info.empty ? 0 : int(snap_.types.size());
        for (int e = 1; e <= n; ++e) {
            const int t = snap_.types[size_t(e - 1)];
            juce::String name = e - 1 < int(info.elements.size()) ? info.elements[size_t(e - 1)].name : juce::String();
            if (t == 0) {  // an insert effect: its effect
                const char* fx = hs2::effectName(int(ctx_.value(e, 0x13) * 128.9f));
                if (name.isEmpty() && fx != nullptr) name = fx;
                else if (fx != nullptr && name != fx) name << " (" << fx << ")";
            }
            titles.add("Element " + juce::String(e) + ": " + elementTypeName(t) + (name.isNotEmpty() ? ", " + name : juce::String()));
        }
        if (titles.isEmpty()) titles.add(info.empty ? "None, the part is empty" : "None, the patch has no elements");
        elemIndex_ = juce::jlimit(0, titles.size() - 1, elemIndex_);
        setBoxItems(elemBox_, titles, elemIndex_);
        std::vector<EditGroup> groups;
        juce::StringArray gtitles;
        if (n > 0)
            for (auto& g : (groups = elementGroups(ctx_, elemIndex_ + 1))) gtitles.add(g.title);
        if (gtitles.isEmpty()) gtitles.add("None");
        elemGroup_ = juce::jlimit(0, gtitles.size() - 1, elemGroup_);
        setBoxItems(elemGroupBox_, gtitles, elemGroup_);
        bindEditRows(elemRows_, elemIndex_ + 1, groups.empty() ? std::vector<int>() : groups[size_t(elemGroup_)].idx,
                     rebindAll);
    } else {
        juce::StringArray titles;
        for (int k = 0; k < kSendFxSlots; ++k) titles.add(sendFxGroup(ctx_, k).title);
        setBoxItems(sendSlotBox_, titles, sendSlot_);
        bindEditRows(sendRows_, kSendFxElem, info.empty ? std::vector<int>() : sendFxGroup(ctx_, sendSlot_).idx, rebindAll);
    }
}

// The rows of a patch editor page for parameters `idx` of element `elem` (PatchEdit.h): a row is bound again only when
// what it shows changes (another parameter, name or list of values), so a focused control is not disturbed.
void HS2Editor::bindEditRows(std::vector<std::unique_ptr<ValueRow>>& rows, int elem, const std::vector<int>& idx,
                             bool rebindAll) {
    bool relayout = false;
    for (size_t r = 0; r < rows.size(); ++r) {
        auto& row = *rows[r];
        if (r >= idx.size()) {
            if (row.bound()) {
                row.unbind();
                relayout = true;
            }
            continue;
        }
        const int i = idx[r];
        const bool ro = editReadOnly(ctx_, elem, i);
        const auto name = editName(ctx_, elem, i);
        const auto title = ro ? name + " (read only)" : name;
        auto choices = editChoices(ctx_, elem, i);
        const auto key = juce::String(elem) + ":" + juce::String(i);
        bool same = !rebindAll && row.bound() && row.binding().key == key && row.binding().title == title &&
                    row.binding().choices.size() == choices.size();
        for (size_t k = 0; same && k < choices.size(); ++k) same = row.binding().choices[k].second == choices[k].second;
        if (same) {
            row.refresh();
            continue;
        }
        ValueRow::Binding b;
        b.key = key;
        b.title = title;
        b.description = name + ".";
        b.readOnly = ro;
        b.choices = std::move(choices);
        b.get = [this, elem, i] { return ctx_.value(elem, i); };
        b.text = [this, elem, i](float v) { return editText(ctx_, elem, i, v); };
        if (!ro) {
            b.set = [this, elem, i](float v) {
                proc_.setEdit(part(), elem, i, v);
                auto it = editsSeen_.find({elem, i});
                if (it == editsSeen_.end()) editsSeen_[{elem, i}] = {v, snap_.get(elem, i)};
                else it->second.value = v;
            };
            b.step = [this, elem, i](float v, int d) { return editStep(ctx_, elem, i, v, d); };
            b.reset = [this, elem, i] {
                proc_.resetEdit(part(), elem, i);
                // shown at once; the next refresh takes the edits and values from the processor again
                if (auto it = editsSeen_.find({elem, i}); it != editsSeen_.end()) it->second.value = it->second.original;
            };
        }
        const bool wasVisible = row.isVisible();
        row.bind(std::move(b));
        relayout = relayout || !wasVisible;
    }
    if (relayout) resized();
}

void HS2Editor::refreshPages() {
    // the effect in each slot, in the slot list
    juce::StringArray names;
    for (int k = 0; k < kGfxSlots; ++k) {
        names.add("Effect " + juce::String(k + 1) + ": " + proc_.gfxSlotText(k));
    }
    if (names != seenFxNames_) {
        seenFxNames_ = names;
        for (int k = 0; k < kGfxSlots; ++k) gfxSlot_.changeItemText(k + 1, names[k]);
        const int slot = juce::jlimit(0, kGfxSlots - 1, proc_.selectedEffectSlot.load());
        gfxSlot_.setSelectedId(slot + 1, juce::dontSendNotification);  // shows the renamed text
        gfxPage_.setTitle("Global effects, " + names[slot]);
        gfxPage_.repaint();
    }
    fillEffectBox();
    // the effect knob rows: the effect's own knob names, choices and texts; unused knobs hidden. A knob rebinds when
    // the effect in the slot changes (a combi) or its table does (Delay follows Sync).
    {
        const int slot = juce::jlimit(0, kGfxSlots - 1, proc_.selectedEffectSlot.load());
        const auto fxName = proc_.globalFxName(slot);
        bool relayout = false;
        for (size_t r = 0; r < gfxRows_.size() && r < gfxKnobSeen_.size(); ++r) {
            const int j = gfxJ_[r];
            if (gfxDef(j).kind != GfxKind::knob) continue;
            const auto* knob = proc_.gfxKnob(slot, j - 1);
            if (knob == gfxKnobSeen_[r]) continue;
            gfxKnobSeen_[r] = knob;
            auto& row = *gfxRows_[r];
            if (knob != nullptr) {
                const auto name = juce::String::fromUTF8(knob->name);
                row.setRowTitle(name, name + " of the " + fxName + " in effect slot " + juce::String(slot + 1) + ".");
            }
            row.bind(gfxIndex(slot, j));
            if (row.isVisible() != (knob != nullptr)) {
                row.setVisible(knob != nullptr);
                relayout = true;
            }
        }
        if (relayout) resized();
    }
    // a tuning scale list follows its bank
    const int p = part();
    for (size_t r = 0; r < tuningRows_.size(); ++r) {
        const auto& d = kExtDefs[tuningE_[r]];
        if (d.kind != ExtKind::tuneScale) continue;
        const int bankE = tuningE_[r] - 1;  // the bank is the setting before the scale
        const int bank = int(proc_.exposed(extIndex(p, bankE)).getValue() * 128.9f);
        if (bank != seenTuneBank_[size_t(d.tuning)]) {
            seenTuneBank_[size_t(d.tuning)] = bank;
            tuningRows_[r]->bind(extIndex(p, tuningE_[r]));
        }
    }
}

void HS2Editor::selectPart(int p) {
    if (p != attachedPart_)
        typing_.releaseAll([this](int c, int note, int v) { sendTypingNote(c, note, v); });
    p = juce::jlimit(0, kNumParts - 1, p);
    proc_.selectedPart = p;
    if (partBox_.getSelectedId() != p + 1) partBox_.setSelectedId(p + 1, juce::dontSendNotification);
    const juce::String n = "Part " + juce::String(p + 1);
    knobs_.setTitle(n + " Hyperknobs");
    mixer_.setTitle(n + " mixer");
    knobs_.repaint();
    mixer_.repaint();
    if (attachedPart_ != p) {
        attachedPart_ = p;
        for (int k = 0; k < kNumKnobs; ++k)
            knobAttach_[size_t(k)] = std::make_unique<juce::SliderParameterAttachment>(proc_.partParam(p, ppKnob1 + k),
                                                                                       knobSliders_[size_t(k)]);
        muteAttach_ = std::make_unique<juce::ButtonParameterAttachment>(proc_.partParam(p, ppMute), mute_);
        levelAttach_ = std::make_unique<juce::SliderParameterAttachment>(proc_.partParam(p, ppLevel), level_);
        panAttach_ = std::make_unique<juce::SliderParameterAttachment>(proc_.partParam(p, ppPan), pan_);
        for (int k = 0; k < kNumSends; ++k)
            sendAttach_[size_t(k)] = std::make_unique<juce::SliderParameterAttachment>(
                proc_.partParam(p, ppSend1 + k), sends_[size_t(k)]);
        // Delete on a slider: back to the value the part's patch loaded with (slider value = parameter value 0..1)
        auto reset = [this, p](KeySlider& s, int pp) {
            s.defaultValue = [this, p, pp] { return double(proc_.loadedValue(exposedIndex(p, pp))); };
        };
        for (int k = 0; k < kNumKnobs; ++k) reset(knobSliders_[size_t(k)], ppKnob1 + k);
        reset(level_, ppLevel);
        reset(pan_, ppPan);
        for (int k = 0; k < kNumSends; ++k) reset(sends_[size_t(k)], ppSend1 + k);
        bindPages();
    }
    for (int k = 0; k < kNumKnobs; ++k) {
        const auto name = proc_.knobName(p, k);
        knobSliders_[size_t(k)].setTitle(name);
        knobLabels_[size_t(k)].setText(name, juce::dontSendNotification);
    }
    const auto info = proc_.partInfo(p);
    const auto text = infoText(p, info);
    const auto sound = info.empty ? juce::String("No sound loaded") : info.name;
    if (currentSound_.getButtonText() != sound) {
        currentSound_.setButtonText(sound);
        if (currentSound_.hasKeyboardFocus(false)) announceTypingSetting(currentSound_, sound);
    }
    if (info_.getText() != text) {
        info_.setText(text, juce::dontSendNotification);
        info_.setCaretPosition(0);
    }
    if (!list_.hasKeyboardFocus(true)) selectCurrentPatchInList();
}

// ---- list model

juce::String HS2Editor::rowText(int row) const {
    if (library_ == nullptr || row < 0 || row >= int(filtered_.size())) return {};
    const auto& ref = library_->patches[size_t(filtered_[size_t(row)])];
    return fromLatin1(ref.name) + (ref.combi ? " (combi)" : "") +
           (ref.captured ? " (captured)" : ref.copyProtected ? " (copy-protected, cannot load)" : "");
}

void HS2Editor::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (library_ == nullptr || row < 0 || row >= int(filtered_.size())) return;
    const auto& ref = library_->patches[size_t(filtered_[size_t(row)])];
    if (selected) g.fillAll(kAccent.withAlpha(0.35f));
    g.setColour(kText);
    g.setFont(uiFont(15.f));
    g.drawText(rowText(row), 8, 0, width / 2, height, juce::Justification::centredLeft);
    if (const auto* b = library_->bank(ref.bank)) {
        g.setColour(kText.withAlpha(0.7f));
        g.drawText(fromLatin1(b->name), width / 2, 0, width / 2 - 8, height, juce::Justification::centredRight);
    }
}

juce::String HS2Editor::getNameForRow(int row) {
    if (library_ == nullptr || row < 0 || row >= int(filtered_.size())) return {};
    const auto& ref = library_->patches[size_t(filtered_[size_t(row)])];
    juce::String s = rowText(row);
    if (bankBox_.getSelectedId() <= 1)
        if (const auto* b = library_->bank(ref.bank)) s << ", " << fromLatin1(b->name);
    return s;
}

void HS2Editor::selectedRowsChanged(int lastRowSelected) {
    if (!suppressSelectionLoad_ && autoLoad_.getToggleState() && lastRowSelected >= 0) loadRow(lastRowSelected, false);
    if (list_.hasKeyboardFocus(true)) list_.focusSelectedRow();
}

void HS2Editor::returnKeyPressed(int row) { loadRow(row, true); }

void HS2Editor::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) { loadRow(row, true); }

// ---- behaviour

void HS2Editor::loadRow(int row, bool explicitRequest) {
    if (library_ == nullptr || row < 0 || row >= int(filtered_.size())) return;
    const auto& ref = library_->patches[size_t(filtered_[size_t(row)])];
    if (ref.combi) {
        proc_.requestCombi(ref.bank, ref.index, explicitRequest);
        status_.setText("Loading combi " + fromLatin1(ref.name) + "...", juce::dontSendNotification);
    } else {
        proc_.requestPatch(part(), ref.bank, ref.index, explicitRequest);
        status_.setText("Loading " + fromLatin1(ref.name) + " into part " + juce::String(part() + 1) + "...",
                        juce::dontSendNotification);
    }
}

void HS2Editor::stepPatch(int delta) {
    if (filtered_.empty()) {
        status_.setText("The patch list is empty.", juce::dontSendNotification);
        return;
    }
    int row = list_.getSelectedRow();
    if (row < 0) row = delta > 0 ? -1 : int(filtered_.size());
    const int target = juce::jlimit(0, int(filtered_.size()) - 1, row + delta);
    if (target == row) {
        status_.setText(delta > 0 ? "Already at the last patch in the list."
                                  : "Already at the first patch in the list.",
                        juce::dontSendNotification);
        return;
    }
    suppressSelectionLoad_ = true;
    list_.selectRow(target);
    list_.scrollToEnsureRowIsOnscreen(target);
    suppressSelectionLoad_ = false;
    loadRow(target, true);
}

void HS2Editor::rebuildBankBox() {
    const int previous = bankBox_.getSelectedId();
    bankBox_.clear(juce::dontSendNotification);
    bankBox_.addItem("All banks", 1);
    if (library_ != nullptr)
        for (size_t i = 0; i < library_->banks.size(); ++i)
            bankBox_.addItem(fromLatin1(library_->banks[i].name), int(i) + 2);
    bankBox_.setSelectedId(previous > 0 && previous <= bankBox_.getNumItems() ? previous : 1,
                           juce::dontSendNotification);
}

void HS2Editor::rebuildList() {
    filtered_.clear();
    if (library_ != nullptr) {
        const auto query = toLatin1(search_.getText().trim());
        const int sel = bankBox_.getSelectedId();
        const BankInfo* onlyBank = sel >= 2 && size_t(sel - 2) < library_->banks.size()
                                       ? &library_->banks[size_t(sel - 2)]
                                       : nullptr;
        auto consider = [&](int i) {
            const auto& ref = library_->patches[size_t(i)];
            if (query.empty()) {
                filtered_.push_back(i);
                return;
            }
            const auto* b = library_->bank(ref.bank);
            if (matchesQuery(ref.name + (ref.combi ? " combi " : " ") + (b ? b->name : std::string()), query))
                filtered_.push_back(i);
        };
        if (onlyBank != nullptr) for (int i : onlyBank->patches) consider(i);
        else for (int i = 0; i < int(library_->patches.size()); ++i) consider(i);
    }
    suppressSelectionLoad_ = true;
    list_.updateContent();
    list_.deselectAllRows();
    selectCurrentPatchInList();
    suppressSelectionLoad_ = false;
    list_.repaint();
    const int n = int(filtered_.size());
    list_.setTitle(n == 0 ? juce::String("Patches, no patches match") : "Patches, " + juce::String(n) + " shown");
}

void HS2Editor::selectCurrentPatchInList() {
    if (library_ == nullptr) return;
    const auto inf = proc_.partInfo(part());
    if (inf.empty || inf.bank < 0) return;
    const int idx = library_->find(inf.bank, inf.index);
    for (size_t r = 0; r < filtered_.size(); ++r) {
        if (filtered_[r] == idx) {
            const bool wasSuppressed = suppressSelectionLoad_;
            suppressSelectionLoad_ = true;
            list_.selectRow(int(r), true, true);
            suppressSelectionLoad_ = wasSuppressed;
            return;
        }
    }
}

juce::String HS2Editor::infoText(int p, const PatchInfo& inf) const {
    juce::String s;
    s << "Part " << (p + 1) << ", MIDI channel " << (p + 1) << ".\n";
    const auto combi = proc_.combiName();
    if (inf.empty) {
        s << "This part is empty. Choose a patch in the list to load it into this part.\n";
    } else {
        s << "Name: " << inf.name << "\n";
        if (inf.bank >= 0) s << "Bank: " << inf.bankName << ", patch " << (inf.index + 1) << "\n";
        else s << "From the combi " << combi << "\n";
        if (inf.description.isNotEmpty()) s << "Description: " << inf.description << "\n";
        if (inf.controllers.isNotEmpty()) s << "Controllers: " << inf.controllers << "\n";
        juce::StringArray knobs;
        for (int k = 0; k < kNumKnobs; ++k)
            if (inf.knobNames[size_t(k)].isNotEmpty()) knobs.add(juce::String(k + 1) + " " + inf.knobNames[size_t(k)]);
        if (!knobs.isEmpty()) s << "Hyperknobs: " << knobs.joinIntoString(", ") << "\n";
        if (inf.author.isNotEmpty()) s << "Author: " << inf.author << "\n";
        if (!inf.warnings.isEmpty()) s << "Not supported yet: " << inf.warnings.joinIntoString("; ") << "\n";
    }
    if (combi.isNotEmpty()) s << "Loaded combi: " << combi << "\n";
    return s.trimEnd();
}

void HS2Editor::refreshFromProcessor() {
    const int change = proc_.changeCounter.load();
    if (change == seenChange_) return;
    seenChange_ = change;

    auto lib = proc_.library();
    if (lib != library_) {
        library_ = lib;
        rebuildBankBox();
        rebuildList();
    }
    refreshPartBox();
    if (part() != attachedPart_) selectPart(part());  // a state restore may have changed it
    else selectPart(part());                         // names and info of the selected part
    const auto status = proc_.statusText();
    if (!proc_.isLoading() || status_.getText().isEmpty()) status_.refreshText(status);
    const auto path = proc_.contentFolder().getFullPathName();
    if (contentPath_.getText() != path) contentPath_.setText(path, juce::dontSendNotification);
    if (midiPrograms_.getToggleState() != proc_.midiProgramChanges())
        midiPrograms_.setToggleState(proc_.midiProgramChanges(), juce::dontSendNotification);
    if (ringOut_.getToggleState() != proc_.ringOut())
        ringOut_.setToggleState(proc_.ringOut(), juce::dontSendNotification);
    if (cutOnLoad_.getToggleState() != proc_.cutOnLoad())
        cutOnLoad_.setToggleState(proc_.cutOnLoad(), juce::dontSendNotification);
    const auto installLabel = proc_.installingPresets() ? "Cancel preset installation" : "Install presets...";
    if (installPresets_.getButtonText() != installLabel) installPresets_.setButtonText(installLabel);

    if (proc_.explicitLoadFailed.exchange(false)) status_.grabKeyboardFocus();  // the screen reader reads it
}

void HS2Editor::timerCallback() {
    if (isShowing()) configureStandaloneWindow();
    refreshKeyboard();
    refreshFromProcessor();
    refreshPages();
    refreshEditPages(false);
    if (page_ == pgGlobal)
        for (auto& r : globalRows_) r->refresh();
    // Keyboard focus must always land somewhere a screen-reader user can Tab from: when this window has OS focus but
    // keyboard focus is nowhere useful, put it in the search box. In the standalone app the whole process is ours, so
    // being the foreground process is enough; inside a DAW only our own window having focus counts, so the DAW's
    // windows never lose focus to the plugin.
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const bool windowActive =
        proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone
            ? juce::Process::isForegroundProcess()
            : (getPeer() != nullptr && getPeer()->isFocused());
    // "Nowhere useful" = nothing, the editor itself, or a component that contains the editor (the standalone
    // window, a host wrapper), none of which Tab can move into the controls from.
    if (windowActive && isShowing() && (focused == nullptr || focused == this || focused->isParentOf(this))) {
        if (view_ == View::instrument) partBox_.grabKeyboardFocus();
        else if (view_ == View::help) helpText_.grabKeyboardFocus();
        else generalTab_.grabKeyboardFocus();
    }
    if (windowActive && isShowing() && view_ == View::instrument
        && juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0)
        offerPresetUpgrade();
}

void HS2Editor::showError(const juce::String& text) {
    status_.setText(text, juce::dontSendNotification);
    status_.grabKeyboardFocus();
}

bool HS2Editor::keyPressed(const juce::KeyPress& key) {
    const auto mods = key.getModifiers();
    if (key.isKeyCode(juce::KeyPress::F1Key) && mods.isShiftDown()
        && proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        checkForUpdates();
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::F1Key) && !mods.isAnyModifierKeyDown()) {
        showKeyboardHelp(true);
        return true;
    }
    using KP = juce::KeyPress;
    if (mods.isAltDown() && !mods.isCtrlDown() && !mods.isCommandDown() && !mods.isShiftDown()) {
        if (key.isKeyCode(KP::F4Key)) {
#if JUCE_WINDOWS
            if (proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
                if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
                return true;
            }
#endif
            return false;
        }
        if (const auto letter = mnemonicLetter(key); letter != 0 && handleMnemonic(letter)) return true;
    }
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto cmdShift = juce::ModifierKeys(juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
    // Ctrl+letter, also when it arrives as its control code (input without a scan code, e.g. some automation).
    auto cmdLetter = [&key](char c) {
        const auto m = key.getModifiers();
        if (!m.isCommandDown() || m.isAltDown() || m.isShiftDown()) return false;
        const int code = key.getKeyCode();
        return mnemonicLetter(key) == c || code == c - 'a' + 1;
    };
    if (cmdLetter('q') && proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
        return true;
    }
    if (cmdLetter('k') && standaloneAudioAvailable() && view_ == View::instrument) {
        toggleKeyboard();
        return true;
    }
    if (key.isKeyCode(KP::escapeKey) && view_ != View::instrument) {
        if (view_ == View::help) closeHelp_.onClick();
        else showView(View::instrument);
        return true;
    }
    if (mods.isCommandDown() && !mods.isAltDown() && !mods.isShiftDown() && key.isKeyCode(',')) {
        showView(View::general);
        return true;
    }
    if (key.isKeyCode(KP::F9Key) && !mods.isAnyModifierKeyDown()) {
        allNotesOff_.onClick();
        return true;
    }
    if (view_ != View::instrument) {
        if (view_ != View::help && audioTab_.isVisible() &&
            (generalTab_.hasKeyboardFocus(false) || audioTab_.hasKeyboardFocus(false)) &&
            (key.isKeyCode(KP::leftKey) || key.isKeyCode(KP::rightKey))) {
            showView(view_ == View::general ? View::audio : View::general);
            return true;
        }
        return false;
    }
#if JUCE_WINDOWS
    if (cmdLetter('h') && hide_.isShowing()) {
        hideToTray();
        return true;
    }
#elif JUCE_MAC
    if (mnemonicLetter(key) == 'h' && mods.isCommandDown() && mods.isShiftDown()
        && !mods.isCtrlDown() && !mods.isAltDown() && hide_.isShowing()) {
        hideToTray();
        return true;
    }
    if (key.isKeyCode('S') && mods.isCtrlDown() && !mods.isCommandDown()
        && !mods.isShiftDown() && !mods.isAltDown()) {
        status_.grabKeyboardFocus();
        return true;
    }
#endif
    if (key.isKeyCode(KP::escapeKey) && keyboard_.getToggleState()) { toggleKeyboard(); return true; }
    if (playKey(key)) return true;
    // Next / previous page: Ctrl+Tab on every platform (on a Mac Cmd+Tab belongs to the system's app switcher).
    const auto ctrl = juce::ModifierKeys::ctrlModifier;
    const auto ctrlShift = juce::ModifierKeys(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier);
    if (key == KP(KP::tabKey, ctrl, 0) || key == KP(KP::tabKey, ctrlShift, 0)) {
        setPage((page_ + (key.getModifiers().isShiftDown() ? int(kNumPages) - 1 : 1)) % int(kNumPages), false);
        focusPage();
        return true;
    }
    if (cmdLetter('f')) {
        search_.grabKeyboardFocus();
        search_.selectAll();
        return true;
    }
    if (cmdLetter('l')) {
        list_.grabKeyboardFocus();
        if (list_.getSelectedRow() < 0 && !filtered_.empty()) {
            suppressSelectionLoad_ = true;
            list_.selectRow(0);
            suppressSelectionLoad_ = false;
        }
        return true;
    }
    if (cmdLetter('i')) {
        if (page_ != pgInfo) setPage(pgInfo, false);
        info_.grabKeyboardFocus();
        return true;
    }
    if (cmdLetter('p')) {
        partBox_.grabKeyboardFocus();
        return true;
    }
    if (key == KP(KP::pageDownKey, cmdShift, 0) || key == KP(KP::pageUpKey, cmdShift, 0)) {
        const int delta = key.isKeyCode(KP::pageDownKey) ? 1 : -1;
        const int target = part() + delta;
        if (target < 0 || target >= kNumParts) {
            status_.setText(delta > 0 ? "Already at part 16." : "Already at part 1.", juce::dontSendNotification);
        } else {
            selectPart(target);
            partBox_.grabKeyboardFocus();  // the screen reader reads the new part and its patch
        }
        return true;
    }
    if (key == KP(KP::pageDownKey, cmd, 0)) {
        stepPatch(1);
        return true;
    }
    if (key == KP(KP::pageUpKey, cmd, 0)) {
        stepPatch(-1);
        return true;
    }
    return false;
}

void HS2Editor::chooseContentFolder() {
    chooser_ = std::make_unique<juce::FileChooser>("Choose the Hypersonic 2 Content folder", proc_.contentFolder());
    juce::Component::SafePointer<HS2Editor> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe](const juce::FileChooser& fc) {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (!f.getChildFile("Patches").isDirectory()) {
                                  safe->showError("The chosen folder has no Patches folder: " +
                                                  f.getFullPathName() +
                                                  ". Choose the folder named Hypersonic 2 Content.");
                                  return;
                              }
                              safe->proc_.setContentFolder(f);
                              safe->status_.setText("Scanning " + f.getFullPathName() + "...",
                                                    juce::dontSendNotification);
                          });
}

void HS2Editor::refreshPresetFolders() {
    for (auto [row, format, defaultFolder] :
         {std::tuple{&vst3Folder_, PresetDestination::vst3, defaultVst3PresetFolder()},
          std::tuple{&nksFolder_, PresetDestination::nks, defaultNksPresetFolder()}}) {
        const auto custom = proc_.presetFolderOverride(format);
        const bool useDefault = custom == juce::File{};
        row->path.setText((useDefault ? defaultFolder : custom).getFullPathName(), false);
        row->reset.setEnabled(!useDefault);
    }
}

void HS2Editor::choosePresetFolder(PresetDestination format) {
    const bool vst3 = format == PresetDestination::vst3;
    auto start = proc_.presetFolderOverride(format);
    if (start == juce::File{}) start = vst3 ? defaultVst3PresetFolder() : defaultNksPresetFolder();
    while (!start.isDirectory() && start.getParentDirectory() != start) start = start.getParentDirectory();
    chooser_ = std::make_unique<juce::FileChooser>(
        vst3 ? "Choose the VST3 preset folder" : "Choose the Komplete Kontrol preset folder", start);
    juce::Component::SafePointer<HS2Editor> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe, format](const juce::FileChooser& fc) {
        if (!safe) return;
        const auto folder = fc.getResult();
        if (folder.isDirectory()) {
            safe->proc_.setPresetFolderOverride(format, folder);
            safe->refreshPresetFolders();
        }
        (format == PresetDestination::vst3 ? safe->vst3Folder_ : safe->nksFolder_).browse.grabKeyboardFocus();
    });
}

void HS2Editor::offerPresetUpgrade() {
    if (checkedPresetUpgrade_ || proc_.wrapperType != juce::AudioProcessor::wrapperType_Standalone
        || proc_.installingPresets()) return;
    const auto library = proc_.library();
    if (library == nullptr || library->patches.empty() || !library->errors.empty()) return;
    checkedPresetUpgrade_ = true;
    if (proc_.presetUpgradeWasOffered(kNksMappingRevision) || !kompleteKontrolInstalled()) return;
    const auto options = proc_.presetInstallOptions();
    if (hasOlderNksPresets(options.nksRoot) && proc_.recordPresetUpgradeOffer(kNksMappingRevision))
        confirmInstallPresets(true);
}

void HS2Editor::confirmInstallPresets(bool upgrade) {
    if (proc_.installingPresets()) {
        proc_.cancelPresetInstall();
        showView(View::instrument);
        status_.grabKeyboardFocus();
        return;
    }
    auto destinations = proc_.presetInstallOptions();
    juce::String msg;
    if (upgrade) {
        destinations.vst3Root = juce::File{};
        destinations.auRoot = juce::File{};
        destinations.requiredExistingRoots = {destinations.nksRoot};
        msg << "An older Hypersonic preset bank is installed for Komplete Kontrol. The update adds more knob pages, "
               "including mixer, tuning, arpeggiator and effects controls, and puts banks and patches in instrument order. "
               "Choose Later to skip; you can update any time from Preferences.\n\n";
    } else {
        msg << "This writes a preset for every patch and combi in your content folder.\n\n"
            << "VST3 presets: " << destinations.vst3Root.getFullPathName() << "\n";
    }
    if (destinations.auRoot != juce::File())
        msg << "Audio Unit presets: " << destinations.auRoot.getFullPathName() << "\n";
    if (destinations.nksRoot != juce::File())
        msg << "Komplete Kontrol presets: " << destinations.nksRoot.getFullPathName() << "\n";
    else
        msg << "Komplete Kontrol is not installed, so no Komplete Kontrol presets are written.\n";
    msg << "\n"
        << "Presets previously installed in these folders are replaced; other presets and old destinations are left "
           "alone. Existing presets stay in place until the new files and previews are ready. "
           "Copy-protected patches without a capture are left out. Progress appears in Status. "
           "To stop generation, choose Cancel preset installation in Preferences.";
    const auto title = upgrade ? "Update Komplete Kontrol presets?" : "Install presets";
    juce::Component::SafePointer<HS2Editor> safe(this);
#if JUCE_MAJOR_VERSION >= 8
    const auto options = juce::MessageBoxOptions()
                             .withIconType(juce::MessageBoxIconType::QuestionIcon)
                             .withTitle(title)
                             .withMessage(msg)
                             .withButton(juce::MessageBoxButtonOptions{upgrade ? "Later" : "Cancel", 0}
                                             .withRole(juce::MessageBoxButtonRole::cancel).withPreferredAction())
                             .withButton(upgrade ? "Update presets" : "Install", 1)
                             .withAssociatedComponent(this);
    constexpr int install = 1;
    juce::NativeMessageBox::showForOptionsAsync(options, [safe, destinations](int result) {
#else  // JUCE 7: a native OK / Cancel box; OK is button index 0
    msg << "\n\nOK installs them.";
    const auto options = juce::MessageBoxOptions()
                             .withIconType(juce::MessageBoxIconType::QuestionIcon)
                             .withTitle(title)
                             .withMessage(msg)
                             .withButton("OK")
                             .withButton(upgrade ? "Later" : "Cancel")
                             .withAssociatedComponent(this);
    constexpr int install = 0;
    juce::NativeMessageBox::showAsync(options, [safe, destinations](int result) {
#endif
        if (safe == nullptr || result != install) return;
        safe->proc_.installPresets(destinations);
        safe->showView(View::instrument);
        safe->status_.grabKeyboardFocus();
    });
}

bool HS2Editor::handleMnemonic(int key) {
#if JUCE_WINDOWS
    auto focus = [](juce::Component& component) {
        if (!component.isShowing() || !component.isEnabled()) return false;
        component.grabKeyboardFocus();
        return true;
    };
    auto press = [](juce::Button& button) {
        if (!button.isShowing() || !button.isEnabled()) return false;
        button.triggerClick();
        return true;
    };
    if (view_ == View::help) return false;
    if (view_ == View::general || view_ == View::audio) {
        if (key == 'g') return press(generalTab_);
        if (key == 'a') return press(audioTab_);
        if (view_ == View::audio) {
            auto* audio = dynamic_cast<AudioMidiSettings*>(audioPanel_.get());
            return audio != nullptr && audio->handleMnemonic(key);
        }
        switch (key) {
            case 'c': return press(chooseContent_);
            case 'p': return focus(memoryMode_);
            case 'm': return press(midiPrograms_);
            case 'e': return press(ringOut_);
            case 's': return press(cutOnLoad_);
            case 'i': return press(installPresets_);
            case 'r': return press(rescan_);
            default: return false;
        }
    }
    switch (key) {
        case 'w': return focus(currentSound_);
        case 'c': return press(clearPart_);
        case 'b': return focus(bankBox_);
        case 'u': return press(loadButton_);
        case 's': return focus(status_);
        case 'o': return press(autoLoad_);
        case 'f': return press(mute_);
        case 'm': return focus(masterVolume_);
        default: return false;
    }
#else
    juce::ignoreUnused(key);
    return false;
#endif
}

void HS2Editor::showKeyboardHelp(bool contextual) {
    const juce::String shortcuts =
        "Ctrl+P: part selector. Up and Down arrows choose the part (part N plays MIDI channel N).\n"
        "Ctrl+Shift+Page Down / Ctrl+Shift+Page Up: next / previous part.\n"
        "Ctrl+F: search patches and combis. Enter in the search box moves to the patch list.\n"
        "Ctrl+L: patch list. Type letters to jump to a patch; Enter loads it into the selected part. A combi loads "
        "into all parts.\n"
        "Ctrl+Page Down / Ctrl+Page Up: next / previous patch.\n"
        "Ctrl+I: information about the selected part's patch.\n"
        "Ctrl+Tab / Ctrl+Shift+Tab: next / previous page (patch and mixer, part settings, arpeggiator, microtuning, "
        "patch parameters, elements, patch send effects, global effects, global settings); the Show box in the Part "
        "section chooses one too.\n"
        "Sliders: arrow keys, Page Up / Page Down, Home / End; Delete returns the slider to the patch's own setting.\n"
        "Patch parameters, Elements and Patch send effects edit the selected part's patch: choose the element and "
        "group at the top of the page. Edits are saved with the project and end when another patch loads.\n"
        "Ctrl+comma: Preferences. F9: All Notes Off. Ctrl+Q: quit the standalone.\n"
        "Ctrl+K: Musical Keyboard in the standalone. Z and X change octave; C and V change velocity.\n"
        "F1: help for the focused control. Shift+F1: check for updates in the standalone.\n"
#if JUCE_WINDOWS
        "\nCtrl+H: hide the standalone to the system tray.\n"
        "\nAlt shortcuts in the instrument: W current sound, C clear part, B bank, U load selected patch, "
        "O load on selection, F mute, M master volume, S status.\n"
        "Preferences: G General, A Audio and MIDI. General: C choose content, P piano memory, "
        "M MIDI programs, E effect tails, S stop old sounds, I install presets, "
        "R rescan. Audio and MIDI: D driver, O output, S sample rate, U buffer, M MIDI inputs, "
        "T status, R refresh, P driver panel.\n"
#endif
#if JUCE_MAC
        "On a Mac, use Cmd for the Ctrl shortcuts above, except Ctrl+Tab and Ctrl+Shift+Tab, which stay Ctrl.\n"
        "Control+S: status. Command+Shift+H: hide the standalone to the menu bar.\n"
#endif
        "In REAPER, turn on \"Send all keyboard input to plug-in\" in the FX window's menu so these keys reach the "
        "plugin.";
    if (view_ != View::help) {
        helpReturn_ = view_;
        returnFocus_ = juce::Component::getCurrentlyFocusedComponent();
    }
    const auto specific = contextual ? controlHelp(returnFocus_.getComponent(), *this) : juce::String();
    helpText_.setText((specific.isNotEmpty() ? specific.replace(". ", ".\n\n") : shortcuts)
                          + "\n\nUse arrows to review, Escape to close.", false);
    helpText_.setCaretPosition(0);
    showView(View::help);
}

void HS2Editor::showView(View view) {
    if (view == View::audio && audioPanel_ == nullptr) {
        auto audio = createAudioMidiSettings();
        if (!audio) return;
        audio->setExplicitFocusOrder(2);
        addAndMakeVisible(*audio);
        audioPanel_ = std::move(audio);
    }
    view_ = view;
    if (view == View::general) refreshPresetFolders();
    refreshKeyboard();
    const bool instrument = view == View::instrument;
    const bool prefs = view == View::general || view == View::audio;
    for (auto* section : {&partSection_, &browser_, &master_, &statusSection_}) section->setVisible(instrument);
    preferencesBar_.setVisible(prefs);
    generalTab_.setToggleState(view == View::general, juce::dontSendNotification);
    audioTab_.setToggleState(view == View::audio, juce::dontSendNotification);
    settings_.setVisible(view == View::general);
    if (audioPanel_ != nullptr) audioPanel_->setVisible(view == View::audio);
    helpPane_.setVisible(view == View::help);
    setPage(proc_.selectedPage.load(), false);
    if (!isShowing()) return;
    if (instrument) preferences_.grabKeyboardFocus();
    else if (view == View::general) generalTab_.grabKeyboardFocus();
    else if (view == View::audio) audioTab_.grabKeyboardFocus();
    else helpText_.grabKeyboardFocus();
}

void HS2Editor::configureStandaloneWindow() {
    if (nativeWindowConfigured_ || proc_.wrapperType != juce::AudioProcessor::wrapperType_Standalone) return;
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>()) {
        nativeWindowConfigured_ = true;
        window->setUsingNativeTitleBar(true);
        window->setTitleBarButtonsRequired(juce::DocumentWindow::allButtons, false);
        window->setTitleBarHeight(0);
    }
}

void HS2Editor::hideToTray() {
#if JUCE_WINDOWS || JUCE_MAC
#if JUCE_MAC
    if (!tray_) tray_ = createMacMenuBarItem([this] { restoreFromTray(); }, [this] {
        restoreFromTray();
        showView(View::general);
    });
#else
    if (!tray_) tray_ = std::make_unique<TrayIcon>([this] { restoreFromTray(); }, [] {
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
    });
#endif
    if (tray_ && tray_->getNativeHandle() != nullptr) {
        getTopLevelComponent()->setVisible(false);
#if JUCE_MAC
        setMacMenuBarHidden(true);
#endif
        refreshKeyboard();
    }
    else showError("Could not create the notification area icon.");
#endif
}

void HS2Editor::sendTypingNote(int channel, int pitch, int velocity) {
    sendStandaloneNote(velocity == 0 ? juce::MidiMessage::noteOff(channel, pitch)
                                   : juce::MidiMessage::noteOn(channel, pitch, juce::uint8(velocity)));
}

void HS2Editor::toggleKeyboard() {
    const bool enable = !keyboard_.getToggleState();
    keyboard_.setToggleState(enable, juce::dontSendNotification);
    if (enable) {
        typing_.start(false);
        octave_.setValue(typing_.octave(), juce::dontSendNotification);
        velocity_.setValue(typing_.velocity(), juce::dontSendNotification);
    }
    startTimerHz(enable ? 60 : 10);
    refreshKeyboard();
}

void HS2Editor::refreshKeyboard() {
    const auto* focus = juce::Component::getCurrentlyFocusedComponent();
    const bool entering = dynamic_cast<const juce::TextEditor*>(focus) != nullptr
        && focus != &info_ && focus != &status_;
    const bool active = keyboard_.getToggleState() && view_ == View::instrument && isShowing()
        && focus != nullptr && isParentOf(focus) && !entering && juce::Process::isForegroundProcess();
    auto send = [this](int c, int p, int v) { sendTypingNote(c, p, v); };
    if (active != typingActive_) {
        typing_.releaseAll(send);
        for (size_t i = 0; i < typingKeys.size(); ++i)
            blockedTypingKeys_[i] = juce::KeyPress::isKeyCurrentlyDown(typingKeys[i].key);
        typingActive_ = active;
    }
    for (size_t i = 0; i < typingKeys.size(); ++i) {
        if ((blockedTypingKeys_[i] || typing_.held(int(i))) && !juce::KeyPress::isKeyCurrentlyDown(typingKeys[i].key)) {
            blockedTypingKeys_[i] = false;
            typing_.release(int(i), send);
        }
    }
    const bool showPanel = keyboard_.getToggleState() && view_ == View::instrument
        && (!entering || octave_.isEntering() || velocity_.isEntering());
    if (keyboardPanel_.isVisible() != showPanel) {
        keyboardPanel_.setVisible(showPanel);
        resized();
    }
}

bool HS2Editor::playKey(const juce::KeyPress& key) {
    if (!keyboard_.getToggleState() || key.getModifiers().isAnyModifierKeyDown()) return false;
    refreshKeyboard();
    if (!typingActive_) return false;
    const auto code = juce::CharacterFunctions::toLowerCase(juce::juce_wchar(key.getKeyCode()));
    const int index = MusicalTyping::keyIndex(code);
    if (index >= 0) {
        if (!blockedTypingKeys_[size_t(index)])
            typing_.press(index, index, part() + 1, [this](int c, int p, int v) { sendTypingNote(c, p, v); });
        return true;
    }
    const bool isOctave = code == 'z' || code == 'x';
    auto& slider = isOctave ? octave_ : velocity_;
    const auto before = slider.getValue();
    if (isOctave) {
        octave_.setValue(typing_.octave() + (code == 'z' ? -1 : 1), juce::sendNotificationSync);
    } else if (code == 'c' || code == 'v') {
        velocity_.setValue(typing_.velocity() + (code == 'c' ? -typingVelocityStep : typingVelocityStep), juce::sendNotificationSync);
    } else return false;
    if (before != slider.getValue())
        if (auto* focus = getCurrentlyFocusedComponent())
            announceTypingSetting(*focus, slider.getTitle() + " " + slider.getTextFromValue(slider.getValue()));
    return true;
}

void HS2Editor::restoreFromTray() {
#if JUCE_MAC
    setMacMenuBarHidden(false);
#endif
    auto* window = getTopLevelComponent();
    window->setVisible(true);
    if (auto* document = dynamic_cast<juce::DocumentWindow*>(window)) document->setMinimised(false);
    window->toFront(true);
    if (view_ == View::instrument) partBox_.grabKeyboardFocus();
    else if (view_ == View::help) helpText_.grabKeyboardFocus();
    else (view_ == View::audio ? audioTab_ : generalTab_).grabKeyboardFocus();
}

void HS2Editor::checkForUpdates() {
    if (checkingUpdates_) return;
    checkingUpdates_ = true;
    updates_.setEnabled(false);
    juce::Thread::launch([safe = juce::Component::SafePointer<HS2Editor>(this)] {
        int status = 0;
        const auto input = juce::URL(kLatestRelease).createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(6000).withNumRedirectsToFollow(0).withStatusCode(&status)
                .withExtraHeaders("User-Agent: HS2-Port\r\nAccept: application/vnd.github+json\r\n"));
        constexpr size_t limit = 256 * 1024;
        juce::MemoryOutputStream bytes;
        if (input && status == 200) bytes.writeFromInputStream(*input, limit + 1);
        juce::var response;
        const bool parsed = bytes.getDataSize() <= limit && juce::JSON::parse(bytes.toUTF8(), response).wasOk();
        const auto tag = parsed ? response.getProperty("tag_name", {}).toString() : juce::String();
        const auto remote = tag.startsWithChar('v') ? releaseVersion(tag.substring(1)) : std::nullopt;
        const auto current = releaseVersion(JucePlugin_VersionString);
        const bool stable = parsed && response.getProperty("draft", {}) == juce::var(false)
                                   && response.getProperty("prerelease", {}) == juce::var(false);
        juce::MessageManager::callAsync([safe, tag, remote, current, stable] {
            if (!safe) return;
            safe->checkingUpdates_ = false;
            safe->updates_.setEnabled(true);
            if (stable && remote && current && *remote <= *current) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                    "Check for updates", "No newer published version was found.", "OK", safe.getComponent());
                return;
            }
            const auto message = stable && remote && current
                ? "Version " + tag.substring(1) + " is available. Open the project's releases page?"
                : juce::String("Could not check for updates. Open the project's releases page instead?");
            juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::InfoIcon, "Check for updates", message,
                "Open releases", "Close", safe.getComponent(),
                juce::ModalCallbackFunction::create([](int result) {
                    if (result != 0) juce::URL(kReleases).launchInDefaultBrowser();
                }));
        });
    });
}

}  // namespace hs2port
