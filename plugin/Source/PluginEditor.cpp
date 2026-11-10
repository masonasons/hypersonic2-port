#include "PluginEditor.h"

#include "AudioSettings.h"
#include "PresetExport.h"

namespace hs2port {

namespace {
const juce::Colour kBackground(0xff1e2126), kPanel(0xff2a2e35), kText(0xfff2f2f2), kAccent(0xff8fc6ff);

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
    t.setDescription(description);
    t.setWantsKeyboardFocus(true);
}

void setupSlider(juce::Slider& s, int order, const juce::String& description) {
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, true, 110, 22);  // read-only box: no extra unnamed tab stop
    s.setExplicitFocusOrder(order);
    s.setWantsKeyboardFocus(true);
    s.setDescription(description);
}

const char* kSliderKeys = "Arrow keys change the value; Page Up and Page Down in larger steps; Home and End go to "
                          "the lowest and highest value; Delete returns to the patch's own setting.";
}  // namespace

// ------------------------------------------------------------------------------------------------ KeySlider

bool KeySlider::keyPressed(const juce::KeyPress& key) {
    if (!key.getModifiers().isAnyModifierKeyDown()) {
        const double lo = getMinimum(), hi = getMaximum();
        const int code = key.getKeyCode();
        double v = getValue();
        bool handled = true;
        if (code == juce::KeyPress::pageUpKey) v += (hi - lo) / 10.0;
        else if (code == juce::KeyPress::pageDownKey) v -= (hi - lo) / 10.0;
        else if (code == juce::KeyPress::homeKey) v = lo;
        else if (code == juce::KeyPress::endKey) v = hi;
        else if ((code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) && defaultValue) v = defaultValue();
        else handled = false;
        if (handled) {
            setValue(juce::jlimit(lo, hi, v), juce::sendNotificationSync);
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
    box_.setDescription(description + " Up and Down arrows choose a value.");
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
    box_.setDescription(description + " Up and Down arrows choose a value.");
    slider_.setTitle(title);
    slider_.setDescription(description + " " + kSliderKeys);
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
    g.setFont(juce::FontOptions(16.f, juce::Font::bold));
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
    setDescription("Hypersonic 2 port, 16 parts. Press F1 for keyboard help.");
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    setWantsKeyboardFocus(true);
    int order = 1;

    // ---- Part
    addAndMakeVisible(partSection_);
    partSection_.setExplicitFocusOrder(order++);
    partSection_.addAndMakeVisible(partBox_);
    partBox_.setTitle("Part");
    partBox_.setDescription("The part the patch browser, Hyperknobs and part mixer work on. Part N plays MIDI "
                            "channel N. Shortcut Ctrl+P; Ctrl+Shift+Page Up and Page Down change the part from anywhere.");
    partBox_.setExplicitFocusOrder(1);
    partBox_.onChange = [this] {
        const int id = partBox_.getSelectedId();
        if (id >= 1 && id <= kNumParts && id - 1 != part()) selectPart(id - 1);
    };
    setupLabel(partLabel_, "Part", partBox_);
    partSection_.addAndMakeVisible(clearPart_);
    clearPart_.setExplicitFocusOrder(2);
    clearPart_.setDescription("Empties the selected part.");
    clearPart_.onClick = [this] {
        if (proc_.partInfo(part()).empty) {
            showError("Part " + juce::String(part() + 1) + " is already empty.");
            return;
        }
        proc_.requestClearPart(part());
    };
    partSection_.addAndMakeVisible(pageBox_);
    pageBox_.setTitle("Show");
    pageBox_.setDescription("What the right side of the window shows for the selected part: its patch, Hyperknobs and "
                            "mixer; its settings; its arpeggiator; its microtuning; or the global effects. Shortcut "
                            "Ctrl+Tab and Ctrl+Shift+Tab.");
    pageBox_.setExplicitFocusOrder(3);
    pageBox_.addItemList({"Patch and mixer", "Part settings", "Arpeggiator", "Microtuning", "Global effects"}, 1);
    pageBox_.onChange = [this] {
        const int id = pageBox_.getSelectedId();
        if (id >= 1 && id - 1 != page_) setPage(id - 1, false);
    };
    setupLabel(pageLabel_, "Show", pageBox_);

    // ---- Patch browser
    addAndMakeVisible(browser_);
    browser_.setExplicitFocusOrder(order++);
    browser_.addAndMakeVisible(search_);
    search_.setTitle("Search patches");
    search_.setDescription("Type words from a patch, combi or bank name. Enter moves to the patch list.");
    search_.setTextToShowWhenEmpty("Search patches and combis", juce::Colours::grey);
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
    bankBox_.setDescription("Show the patches of one bank, or of all banks. The combi banks hold combis.");
    bankBox_.setExplicitFocusOrder(2);
    bankBox_.onChange = [this] { rebuildList(); };
    setupLabel(bankLabel_, "Bank", bankBox_);

    browser_.addAndMakeVisible(list_);
    list_.setModel(this);
    list_.setRowHeight(24);
    list_.setTitle("Patches");
    list_.setDescription("Type letters to jump to a patch. Enter loads the selected patch into the selected part; "
                         "a combi loads into all parts.");
    list_.setExplicitFocusOrder(3);
    list_.setColour(juce::ListBox::backgroundColourId, kBackground);
    list_.rowName = [this](int row) {
        return (library_ != nullptr && row >= 0 && row < int(filtered_.size()))
                   ? fromLatin1(library_->patches[size_t(filtered_[size_t(row)])].name)
                   : juce::String();
    };

    for (auto* b : {&loadButton_, &prevButton_, &nextButton_}) browser_.addAndMakeVisible(*b);
    loadButton_.setExplicitFocusOrder(4);
    prevButton_.setExplicitFocusOrder(5);
    nextButton_.setExplicitFocusOrder(6);
    loadButton_.setDescription("Loads the patch selected in the list into the selected part, or the selected combi.");
    prevButton_.setDescription("Loads the previous patch in the list. Shortcut Ctrl+Page Up.");
    nextButton_.setDescription("Loads the next patch in the list. Shortcut Ctrl+Page Down.");
    loadButton_.onClick = [this] {
        if (list_.getSelectedRow() >= 0) loadRow(list_.getSelectedRow(), true);
        else showError("No patch is selected in the list.");
    };
    prevButton_.onClick = [this] { stepPatch(-1); };
    nextButton_.onClick = [this] { stepPatch(1); };

    browser_.addAndMakeVisible(autoLoad_);
    autoLoad_.setExplicitFocusOrder(7);
    autoLoad_.setToggleState(true, juce::dontSendNotification);
    autoLoad_.setDescription("When on, moving through the patch list loads each patch so you can play it at once. "
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
    mute_.setDescription("Silences the selected part.");
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
    makePage(arpPage_, ExtPage::arp, arpRows_, arpE_);
    makePage(tuningPage_, ExtPage::tuning, tuningRows_, tuningE_);
    addChildComponent(gfxPage_);
    gfxPage_.setExplicitFocusOrder(order++);
    gfxPage_.addAndMakeVisible(gfxSlot_);
    gfxSlot_.setTitle("Effect slot");
    gfxSlot_.setDescription("Which of the four global send effects the controls below work on. A combi sets them.");
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
    gfxEffect_.setDescription("The effect in this slot, from the effect presets in the content's Effects folder (as the "
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
    chooseContent_.setDescription("Opens a folder chooser for the Hypersonic 2 Content folder.");
    chooseContent_.onClick = [this] { chooseContentFolder(); };
    settings_.addAndMakeVisible(memoryMode_);
    memoryMode_.setTitle("Piano memory mode");
    memoryMode_.setDescription("Sample set for the piano patches, for every part.");
    memoryMode_.addItem("ECO, smallest piano samples", 1);
    memoryMode_.addItem("MID, medium piano samples", 2);
    memoryMode_.addItem("XXL, full piano samples", 3);
    memoryMode_.setSelectedId(proc_.memoryMode() + 1, juce::dontSendNotification);
    memoryMode_.setExplicitFocusOrder(3);
    memoryMode_.onChange = [this] { proc_.setMemoryMode(memoryMode_.getSelectedId() - 1); };
    setupLabel(memoryLabel_, "Piano memory", memoryMode_);
    settings_.addAndMakeVisible(midiPrograms_);
    midiPrograms_.setExplicitFocusOrder(4);
    midiPrograms_.setDescription("When on, MIDI bank select (controller 32 = bank number) and program change load a "
                                 "patch into the part of that MIDI channel; banks 120 and up load combis.");
    midiPrograms_.setToggleState(proc_.midiProgramChanges(), juce::dontSendNotification);
    midiPrograms_.onClick = [this] { proc_.setMidiProgramChanges(midiPrograms_.getToggleState()); };
    settings_.addAndMakeVisible(ringOut_);
    ringOut_.setExplicitFocusOrder(5);
    ringOut_.setDescription("When on, delay and reverb tails play out after the last note. The original Hypersonic 2 "
                            "cuts some of them off; turn this off to hear it exactly as the original.");
    ringOut_.setToggleState(proc_.ringOut(), juce::dontSendNotification);
    ringOut_.onClick = [this] { proc_.setRingOut(ringOut_.getToggleState()); };
    settings_.addAndMakeVisible(cutOnLoad_);
    cutOnLoad_.setExplicitFocusOrder(6);
    cutOnLoad_.setDescription("When on, loading a patch or combi stops the notes and effect tails of the part's "
                              "previous patch (with a short fade). When off, they sound on until they end.");
    cutOnLoad_.setToggleState(proc_.cutOnLoad(), juce::dontSendNotification);
    cutOnLoad_.onClick = [this] { proc_.setCutOnLoad(cutOnLoad_.getToggleState()); };
    settings_.addAndMakeVisible(installPresets_);
    installPresets_.setExplicitFocusOrder(7);
    installPresets_.setDescription("Writes a preset file for every patch and combi in your content folder, so you can "
                                   "choose them in your DAW's preset browser and in Komplete Kontrol. Presets you saved "
                                   "yourself are kept.");
    installPresets_.onClick = [this] { confirmInstallPresets(); };
    if (standaloneAudioAvailable()) {  // the standalone app: its audio driver, device, buffer and MIDI inputs
        settings_.addAndMakeVisible(audioSettings_);
        audioSettings_.setExplicitFocusOrder(8);
        audioSettings_.setDescription("Opens the audio driver (ASIO or Windows Audio), output device, sample rate, "
                                      "buffer size and MIDI input settings of this standalone app.");
        audioSettings_.onClick = [this] { showAudioMidiSettings(this); };
    }
    settings_.addAndMakeVisible(help_);
    help_.setExplicitFocusOrder(9);
    help_.setDescription("Lists the keyboard shortcuts. Shortcut F1.");
    help_.onClick = [this] { showKeyboardHelp(); };

    // ---- Status
    addAndMakeVisible(statusSection_);
    statusSection_.setExplicitFocusOrder(order++);
    statusSection_.addAndMakeVisible(status_);
    setupReadOnly(status_, "Status", "Loading progress, errors and unsupported features of the loaded patches.", true);

    for (auto* t : {&search_, &info_, &contentPath_, &status_}) {
        t->setColour(juce::TextEditor::backgroundColourId, kBackground);
        t->setColour(juce::TextEditor::textColourId, kText);
        t->setFont(juce::FontOptions(15.f));
    }

    setResizable(true, true);
    setResizeLimits(1000, 800, 2000, 1600);
    setSize(1200, 860);
    refreshPartBox();
    selectPart(part());
    refreshFromProcessor();
    setPage(proc_.selectedPage.load(), false);  // the page shown last time
    startTimerHz(10);
}

HS2Editor::~HS2Editor() {
    stopTimer();
    list_.setModel(nullptr);
}

void HS2Editor::visibilityChanged() {
    if (isShowing() && !focusedOnce_) {
        focusedOnce_ = true;
        juce::Component::SafePointer<HS2Editor> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) safe->search_.grabKeyboardFocus();
        });
    }
}

void HS2Editor::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void HS2Editor::focusGained(FocusChangeType) {
    // The editor itself is not a useful stop (Tab cannot move on from it): hand focus to the search box.
    juce::Component::SafePointer<HS2Editor> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe != nullptr && safe->hasKeyboardFocus(false)) safe->search_.grabKeyboardFocus();
    });
}

void HS2Editor::resized() {
    // Two columns: the part and the patch browser on the left (the list gets the height), the selected part's
    // details, the master, settings and status on the right.
    auto all = getLocalBounds().reduced(8);
    const int gap = 8;
    auto r = all.removeFromRight(juce::jmax(560, all.getWidth() * 58 / 100));
    all.removeFromRight(gap);
    auto partArea = all.removeFromTop(98);
    all.removeFromTop(gap);
    partSection_.setBounds(partArea);
    browser_.setBounds(all);
    auto statusArea = r.removeFromBottom(90);
    r.removeFromBottom(gap);
    auto settingsArea = r.removeFromBottom(258);
    r.removeFromBottom(gap);
    auto masterArea = r.removeFromBottom(64);
    r.removeFromBottom(gap);
    auto mixerArea = r.removeFromBottom(182);
    r.removeFromBottom(gap);
    auto knobsArea = r.removeFromBottom(146);
    r.removeFromBottom(gap);
    auto currentArea = r;
    current_.setBounds(currentArea);
    knobs_.setBounds(knobsArea);
    mixer_.setBounds(mixerArea);
    // the other pages: the whole area of current patch, Hyperknobs and mixer
    const auto pageArea = currentArea.getUnion(mixerArea);
    for (auto* sec : {&settingsPage_, &arpPage_, &tuningPage_, &gfxPage_}) sec->setBounds(pageArea);
    {
        std::vector<ParamRow*> rows;
        for (auto& r2 : settingsRows_) rows.push_back(r2.get());
        if (linkRow_->isVisible()) rows.push_back(linkRow_.get());
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
    }
    master_.setBounds(masterArea);
    settings_.setBounds(settingsArea);
    statusSection_.setBounds(statusArea);

    {
        auto c = partSection_.content().withHeight(28);
        c.removeFromLeft(60);
        partBox_.setBounds(c.removeFromLeft(juce::jmin(420, c.getWidth() - 200)));
        c.removeFromLeft(10);
        clearPart_.setBounds(c.removeFromLeft(140));
        auto c2 = partSection_.content().withTrimmedTop(34).withHeight(28);
        c2.removeFromLeft(60);
        pageBox_.setBounds(c2.removeFromLeft(juce::jmin(420, c2.getWidth() - 200)));
    }
    {
        auto c = browser_.content();
        auto row = c.removeFromTop(28);
        row.removeFromLeft(60);
        search_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 30));
        row.removeFromLeft(60);
        bankBox_.setBounds(row);
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
            juce::Rectangle<int> cell(c.getX() + col * colW, c.getY() + rowIdx * 36, colW - 10, 30);
            knobSliders_[size_t(k)].setBounds(cell.withTrimmedLeft(150));
        }
    }
    {
        auto c = mixer_.content();
        const int colW = c.getWidth() / 2;
        auto cell = [&](int col, int row) {
            return juce::Rectangle<int>(c.getX() + col * colW, c.getY() + row * 36, colW - 10, 30);
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
        help_.setBounds(row2.removeFromRight(200));
        row2.removeFromRight(6);
        if (audioSettings_.isVisible()) {
            audioSettings_.setBounds(row2.removeFromRight(230));
            row2.removeFromRight(6);
        }
        memoryMode_.setBounds(row2.removeFromLeft(320));
        c.removeFromTop(8);
        midiPrograms_.setBounds(c.removeFromTop(30).withTrimmedLeft(120));
        c.removeFromTop(6);
        ringOut_.setBounds(c.removeFromTop(30).withTrimmedLeft(120));
        c.removeFromTop(6);
        cutOnLoad_.setBounds(c.removeFromTop(30).withTrimmedLeft(120));
        c.removeFromTop(6);
        installPresets_.setBounds(c.removeFromTop(30).withTrimmedLeft(120).withWidth(230));
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
void HS2Editor::layoutRows(Section& s, const std::vector<ParamRow*>& rows,
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
    page_ = juce::jlimit(0, 4, page);
    proc_.selectedPage = page_;
    if (pageBox_.getSelectedId() != page_ + 1) pageBox_.setSelectedId(page_ + 1, juce::dontSendNotification);
    current_.setVisible(page_ == 0);
    knobs_.setVisible(page_ == 0);
    mixer_.setVisible(page_ == 0);
    settingsPage_.setVisible(page_ == 1);
    arpPage_.setVisible(page_ == 2);
    tuningPage_.setVisible(page_ == 3);
    gfxPage_.setVisible(page_ == 4);
    resized();
    if (focusSelector) pageBox_.grabKeyboardFocus();
}

// The first control of the shown page (Ctrl+Tab goes there, so the screen reader reads the page and that control).
void HS2Editor::focusPage() {
    juce::Component* first = nullptr;
    switch (page_) {
        case 0: first = &info_; break;
        case 4: first = &gfxSlot_; break;
        default: {
            auto& rows = page_ == 1 ? settingsRows_ : page_ == 2 ? arpRows_ : tuningRows_;
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
    for (size_t r = 0; r < settingsRows_.size(); ++r) settingsRows_[r]->bind(extIndex(p, settingsE_[r]));
    for (size_t r = 0; r < arpRows_.size(); ++r) arpRows_[r]->bind(extIndex(p, arpE_[r]));
    for (size_t r = 0; r < tuningRows_.size(); ++r) tuningRows_[r]->bind(extIndex(p, tuningE_[r]));
    linkRow_->setVisible(p > 0);  // part 1 has no part above it
    linkRow_->bind(p > 0 ? linkIndex(p) : -1);
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
    for (auto* sec : {&settingsPage_, &arpPage_, &tuningPage_, &gfxPage_}) sec->repaint();
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
    const auto text = infoText(p, proc_.partInfo(p));
    if (info_.getText() != text) info_.setText(text, juce::dontSendNotification);
    if (!list_.hasKeyboardFocus(true)) selectCurrentPatchInList();
}

// ---- list model

juce::String HS2Editor::rowText(int row) const {
    if (library_ == nullptr || row < 0 || row >= int(filtered_.size())) return {};
    const auto& ref = library_->patches[size_t(filtered_[size_t(row)])];
    return fromLatin1(ref.name) + (ref.combi ? " (combi)" : "") +
           (ref.copyProtected ? " (copy-protected, cannot load)" : "");
}

void HS2Editor::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (library_ == nullptr || row < 0 || row >= int(filtered_.size())) return;
    const auto& ref = library_->patches[size_t(filtered_[size_t(row)])];
    if (selected) g.fillAll(kAccent.withAlpha(0.35f));
    g.setColour(kText);
    g.setFont(juce::FontOptions(15.f));
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
    const int target = row + delta;
    if (target < 0 || target >= int(filtered_.size())) {
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
    if (!proc_.isLoading() || status_.getText().isEmpty()) status_.setText(status, juce::dontSendNotification);
    const auto path = proc_.contentFolder().getFullPathName();
    if (contentPath_.getText() != path) contentPath_.setText(path, juce::dontSendNotification);
    if (midiPrograms_.getToggleState() != proc_.midiProgramChanges())
        midiPrograms_.setToggleState(proc_.midiProgramChanges(), juce::dontSendNotification);
    if (ringOut_.getToggleState() != proc_.ringOut())
        ringOut_.setToggleState(proc_.ringOut(), juce::dontSendNotification);
    if (cutOnLoad_.getToggleState() != proc_.cutOnLoad())
        cutOnLoad_.setToggleState(proc_.cutOnLoad(), juce::dontSendNotification);

    if (proc_.explicitLoadFailed.exchange(false)) status_.grabKeyboardFocus();  // the screen reader reads it
}

void HS2Editor::timerCallback() {
    refreshFromProcessor();
    refreshPages();
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
    if (windowActive && isShowing() && (focused == nullptr || focused == this || focused->isParentOf(this)))
        search_.grabKeyboardFocus();
}

void HS2Editor::showError(const juce::String& text) {
    status_.setText(text, juce::dontSendNotification);
    status_.grabKeyboardFocus();
}

bool HS2Editor::keyPressed(const juce::KeyPress& key) {
    if (key.isKeyCode(juce::KeyPress::F1Key)) {
        showKeyboardHelp();
        return true;
    }
    using KP = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto cmdShift = juce::ModifierKeys(juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
    // Ctrl+letter, also when it arrives as its control code (input without a scan code, e.g. some automation).
    auto cmdLetter = [&key](char c) {
        const auto m = key.getModifiers();
        if (!m.isCommandDown() || m.isAltDown() || m.isShiftDown()) return false;
        const int code = key.getKeyCode();
        return juce::CharacterFunctions::toLowerCase(juce::juce_wchar(code)) == juce::juce_wchar(c) ||
               code == c - 'a' + 1;
    };
    if (key == KP(KP::tabKey, cmd, 0) || key == KP(KP::tabKey, cmdShift, 0)) {  // next / previous page
        setPage((page_ + (key.getModifiers().isShiftDown() ? 4 : 1)) % 5, false);
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

void HS2Editor::confirmInstallPresets() {
    if (proc_.installingPresets()) {
        status_.grabKeyboardFocus();
        return;
    }
    juce::String msg;
    msg << "This writes a preset for every patch and combi in your content folder.\n\n"
        << "VST3 presets: " << defaultVst3PresetFolder().getFullPathName() << "\n";
    if (defaultAuPresetFolder() != juce::File())
        msg << "Audio Unit presets: " << defaultAuPresetFolder().getFullPathName() << "\n";
    if (kompleteKontrolInstalled())
        msg << "Komplete Kontrol presets: " << defaultNksPresetFolder().getFullPathName() << "\n";
    else
        msg << "Komplete Kontrol is not installed, so no Komplete Kontrol presets are written.\n";
    msg << "\n"
        << "Presets installed here before are replaced; presets you saved yourself are kept. Copy-protected patches "
           "are left out. Progress and the result appear in the Status box.";
    const auto options = juce::MessageBoxOptions()
                             .withIconType(juce::MessageBoxIconType::QuestionIcon)
                             .withTitle("Install presets")
                             .withMessage(msg)
                             .withButton("Install", 1)
                             .withButton("Cancel", 0)
                             .withAssociatedComponent(this);
    juce::Component::SafePointer<HS2Editor> safe(this);
    juce::NativeMessageBox::showForOptionsAsync(options, [safe](int result) {
        if (safe == nullptr || result != 1) return;
        safe->proc_.installPresets();
        safe->status_.grabKeyboardFocus();
    });
}

void HS2Editor::showKeyboardHelp() {
    const juce::String text =
        "Tab and Shift+Tab move between controls.\n"
        "Ctrl+P: part selector. Up and Down arrows choose the part (part N plays MIDI channel N).\n"
        "Ctrl+Shift+Page Down / Ctrl+Shift+Page Up: next / previous part.\n"
        "Ctrl+F: search patches and combis. Enter in the search box moves to the patch list.\n"
        "Ctrl+L: patch list. Type letters to jump to a patch; Enter loads it into the selected part. A combi loads "
        "into all parts.\n"
        "Ctrl+Page Down / Ctrl+Page Up: next / previous patch.\n"
        "Ctrl+I: information about the selected part's patch.\n"
        "Ctrl+Tab / Ctrl+Shift+Tab: next / previous page (patch and mixer, part settings, arpeggiator, microtuning, "
        "global effects); the Show box in the Part section chooses one too.\n"
        "Sliders: arrow keys, Page Up / Page Down, Home / End; Delete returns the slider to the patch's own setting.\n"
        "F1: this help.\n"
        "In REAPER, turn on \"Send all keyboard input to plug-in\" in the FX window's menu so these keys reach the "
        "plugin.";
    juce::NativeMessageBox::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "HS2 Port keyboard help", text,
                                                this);
}

}  // namespace hs2port
