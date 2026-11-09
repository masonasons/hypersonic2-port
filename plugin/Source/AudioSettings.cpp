#include "AudioSettings.h"

#if JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace hs2port {

namespace {
void hiddenLabel(juce::Label& l, const juce::String& text, juce::Component& target) {
    l.setText(text, juce::dontSendNotification);
    l.attachToComponent(&target, true);
    l.setAccessible(false);  // the control carries the same title
}

void titled(juce::Component& c, const juce::String& title, const juce::String& description, int order) {
    c.setTitle(title);
    c.setDescription(description);
    c.setWantsKeyboardFocus(true);
    c.setExplicitFocusOrder(order);
}

juce::String msText(double samples, double rate) {
    return rate > 0 ? juce::String(samples * 1000.0 / rate, 1) + " ms" : juce::String();
}
}  // namespace

AudioMidiSettings::AudioMidiSettings(juce::AudioDeviceManager& devices, std::function<void()> changed)
    : devices_(devices), changed_(std::move(changed)) {
    setTitle("Audio and MIDI settings");
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);

    addAndMakeVisible(driver_);
    titled(driver_, "Audio driver",
           "ASIO uses your audio interface's own low-latency driver; Windows Audio works with any device. "
           "Changing the driver briefly interrupts the sound.", 1);
    hiddenLabel(driverLabel_, "Audio driver", driver_);
    driver_.onChange = [this] {
        const auto type = driver_.getText();
        if (type.isNotEmpty() && type != devices_.getCurrentAudioDeviceType()) {
            error_.clear();
            devices_.setCurrentAudioDeviceType(type, true);
            if (devices_.getCurrentAudioDevice() == nullptr) error_ = "No " + type + " output device could be opened.";
            if (changed_) changed_();
        }
        update();
    };

    addAndMakeVisible(output_);
    titled(output_, "Output device", "The audio output this app plays through. No audio input is used.", 2);
    hiddenLabel(outputLabel_, "Output device", output_);
    output_.onChange = [this] { applyDevice(0); };

    addAndMakeVisible(rate_);
    titled(rate_, "Sample rate", "The sample rates the output device supports.", 3);
    hiddenLabel(rateLabel_, "Sample rate", rate_);
    rate_.onChange = [this] { applyDevice(1); };

    addAndMakeVisible(buffer_);
    titled(buffer_, "Buffer size",
           "Smaller buffers play notes with less delay but need more processing power; raise it if the sound crackles.", 4);
    hiddenLabel(bufferLabel_, "Buffer size", buffer_);
    buffer_.onChange = [this] { applyDevice(2); };

    addAndMakeVisible(refresh_);
    titled(refresh_, "Refresh devices", "Looks for audio devices and MIDI inputs again.", 5);
    refresh_.onClick = [this] {
        if (auto* type = devices_.getCurrentDeviceTypeObject()) type->scanForDevices();
        rebuildMidiInputs();
        update();
    };

    addAndMakeVisible(panel_);
    titled(panel_, "Driver control panel",
           "Opens the ASIO driver's own settings window. Its accessibility depends on the manufacturer.", 6);
    panel_.onClick = [this] {
        if (auto* device = devices_.getCurrentAudioDevice())
            if (device->hasControlPanel() && device->showControlPanel()) {  // the driver asks for a restart
                devices_.closeAudioDevice();
                devices_.restartLastAudioDevice();
            }
        update();
    };

    addAndMakeVisible(midiGroup_);
    midiGroup_.setTitle("MIDI inputs");
    midiGroup_.setExplicitFocusOrder(7);
    midiLabel_.setText("MIDI inputs (the app plays every input that is on, on all 16 channels)", juce::dontSendNotification);
    midiLabel_.setAccessible(false);
    addAndMakeVisible(midiLabel_);

    addAndMakeVisible(status_);
    status_.setMultiLine(true);
    status_.setReadOnly(true);
    status_.setScrollbarsShown(true);
    status_.setCaretVisible(true);  // so screen readers can review it line by line
    titled(status_, "Audio status", "The audio device in use, its sample rate, buffer and latency, and any error.", 8);
    statusLabel_.setText("Status", juce::dontSendNotification);
    statusLabel_.setAccessible(false);
    addAndMakeVisible(statusLabel_);

    addAndMakeVisible(close_);
    titled(close_, "Close", "Closes this window. Settings are saved as they change. Escape closes it too.", 9);
    close_.onClick = [this] {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    };

    devices_.addChangeListener(this);
    rebuildMidiInputs();
    update();
}

AudioMidiSettings::~AudioMidiSettings() { devices_.removeChangeListener(this); }

void AudioMidiSettings::focusFirst() {
    juce::Component::SafePointer<AudioMidiSettings> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe != nullptr) safe->driver_.grabKeyboardFocus();
    });
}

void AudioMidiSettings::paint(juce::Graphics& g) {
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioMidiSettings::resized() {
    auto r = getLocalBounds().reduced(12);
    const int labelW = 130;
    auto row = [&](juce::Component& c) {
        auto line = r.removeFromTop(30);
        c.setBounds(line.withTrimmedLeft(labelW));
        r.removeFromTop(8);
    };
    row(driver_);
    row(output_);
    row(rate_);
    row(buffer_);
    {
        auto line = r.removeFromTop(30).withTrimmedLeft(labelW);
        refresh_.setBounds(line.removeFromLeft(170));
        line.removeFromLeft(10);
        panel_.setBounds(line.removeFromLeft(220));
        r.removeFromTop(12);
    }
    midiLabel_.setBounds(r.removeFromTop(24));
    const int midiH = std::max(1, int(midiToggles_.size())) * 28;
    midiGroup_.setBounds(r.removeFromTop(midiH));
    for (size_t i = 0; i < midiToggles_.size(); ++i)
        midiToggles_[i]->setBounds(0, int(i) * 28, midiGroup_.getWidth(), 26);
    r.removeFromTop(10);
    close_.setBounds(r.removeFromBottom(30).removeFromRight(120));
    r.removeFromBottom(8);
    statusLabel_.setBounds(r.removeFromTop(22));
    status_.setBounds(r);
}

void AudioMidiSettings::changeListenerCallback(juce::ChangeBroadcaster*) { update(); }

void AudioMidiSettings::fill(juce::ComboBox& box, const juce::StringArray& items, int selected) {
    juce::StringArray old;
    for (int i = 0; i < box.getNumItems(); ++i) old.add(box.getItemText(i));
    if (old != items) {
        box.clear(juce::dontSendNotification);
        box.addItemList(items, 1);
    }
    box.setSelectedItemIndex(selected, juce::dontSendNotification);
    box.setEnabled(!items.isEmpty());
}

void AudioMidiSettings::update() {
    juce::StringArray types;
    for (auto* t : devices_.getAvailableDeviceTypes()) types.add(t->getTypeName());
    fill(driver_, types, types.indexOf(devices_.getCurrentAudioDeviceType()));

    const auto setup = devices_.getAudioDeviceSetup();
    auto* type = devices_.getCurrentDeviceTypeObject();
    const auto outputs = type != nullptr ? type->getDeviceNames(false) : juce::StringArray();
    fill(output_, outputs, outputs.indexOf(setup.outputDeviceName));

    auto* device = devices_.getCurrentAudioDevice();
    juce::StringArray rateItems, bufferItems;
    rates_.clear();
    bufferSizes_.clear();
    int rateSel = -1, bufferSel = -1;
    const double rate = device != nullptr ? device->getCurrentSampleRate() : 0.0;
    if (device != nullptr) {
        for (double r : device->getAvailableSampleRates()) {
            if (r == rate) rateSel = rateItems.size();
            rates_.push_back(r);
            rateItems.add(juce::String(juce::roundToInt(r)) + " Hz");
        }
        for (int b : device->getAvailableBufferSizes()) {
            if (b == device->getCurrentBufferSizeSamples()) bufferSel = bufferItems.size();
            bufferSizes_.push_back(b);
            bufferItems.add(juce::String(b) + " samples, " + msText(b, rate));
        }
    }
    fill(rate_, rateItems, rateSel);
    fill(buffer_, bufferItems, bufferSel);
    panel_.setEnabled(device != nullptr && device->hasControlPanel());

    juce::String s;
    if (error_.isNotEmpty()) s << "Error: " << error_ << "\n";
    if (device != nullptr) {
        s << "Playing through " << device->getName() << " (" << devices_.getCurrentAudioDeviceType() << "), "
          << juce::roundToInt(rate) << " Hz, buffer " << device->getCurrentBufferSizeSamples() << " samples ("
          << msText(device->getCurrentBufferSizeSamples(), rate) << ").\n";
        s << "Output latency " << msText(device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples(), rate)
          << ".\n";
    } else {
        s << "No audio output device is open. Choose a driver and an output device.\n";
    }
    juce::StringArray on;
    for (const auto& m : midiDevices_)
        if (devices_.isMidiInputDeviceEnabled(m.identifier)) on.add(m.name);
    if (midiDevices_.isEmpty())
        s << "No MIDI inputs found. Connect one and choose Refresh devices.";
    else
        s << (on.isEmpty() ? juce::String("No MIDI input is on.") : "MIDI inputs on: " + on.joinIntoString(", ") + ".");
    status_.setText(s.trimEnd(), false);
}

void AudioMidiSettings::applyDevice(int field) {
    auto setup = devices_.getAudioDeviceSetup();
    setup.inputDeviceName.clear();
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    setup.useDefaultOutputChannels = true;
    if (field == 0) {
        if (output_.getText().isEmpty() || output_.getText() == setup.outputDeviceName) return;
        setup.outputDeviceName = output_.getText();
        setup.sampleRate = 0;
        setup.bufferSize = 0;
    } else if (field == 1) {
        const int i = rate_.getSelectedItemIndex();
        if (i < 0 || i >= int(rates_.size()) || rates_[size_t(i)] == setup.sampleRate) return;
        setup.sampleRate = rates_[size_t(i)];
    } else {
        const int i = buffer_.getSelectedItemIndex();
        if (i < 0 || i >= int(bufferSizes_.size()) || bufferSizes_[size_t(i)] == setup.bufferSize) return;
        setup.bufferSize = bufferSizes_[size_t(i)];
    }
    error_ = devices_.setAudioDeviceSetup(setup, true);
    if (changed_) changed_();
    update();
}

void AudioMidiSettings::rebuildMidiInputs() {
    midiDevices_ = juce::MidiInput::getAvailableDevices();
    midiToggles_.clear();
    int order = 1;
    for (const auto& m : midiDevices_) {
        auto t = std::make_unique<juce::ToggleButton>("MIDI input: " + m.name);
        titled(*t, "MIDI input: " + m.name, "When on, this app plays what comes in on this MIDI input.", order++);
        t->setToggleState(devices_.isMidiInputDeviceEnabled(m.identifier), juce::dontSendNotification);
        const auto id = m.identifier;
        auto* raw = t.get();
        t->onClick = [this, id, raw] {
            devices_.setMidiInputDeviceEnabled(id, raw->getToggleState());
            if (changed_) changed_();
            update();
        };
        midiGroup_.addAndMakeVisible(*t);
        midiToggles_.push_back(std::move(t));
    }
    // the window grows with the number of MIDI inputs
    setSize(getWidth() > 0 ? getWidth() : 620, 420 + int(std::max<size_t>(1, midiToggles_.size())) * 28);
    resized();
}

bool standaloneAudioAvailable() {
#if JucePlugin_Build_Standalone
    return juce::StandalonePluginHolder::getInstance() != nullptr;
#else
    return false;
#endif
}

void showAudioMidiSettings([[maybe_unused]] juce::Component* centreAround) {
#if JucePlugin_Build_Standalone
    auto* holder = juce::StandalonePluginHolder::getInstance();
    if (holder == nullptr) return;
    auto content = std::make_unique<AudioMidiSettings>(holder->deviceManager, [] {
        if (auto* h = juce::StandalonePluginHolder::getInstance()) h->saveAudioDeviceState();
    });
    content->focusFirst();
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(content.release());
    o.dialogTitle = "Audio and MIDI settings";
    o.dialogBackgroundColour = o.content->getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = centreAround;
    o.launchAsync();
#endif
}

}  // namespace hs2port
