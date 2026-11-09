#pragma once
// Audio and MIDI settings of the standalone app, built for screen-reader use (the same rules as PluginEditor.h):
// stock JUCE widgets with titles, labels hidden from accessibility, a fixed Tab order, no spoken announcements; the
// current state and any error are in a read-only text box. Replaces JUCE's own settings dialog for keyboard users
// (the approach of OnjLouis/sound-canvas-bridge's audio preferences, MIT licence; written for this plugin).
#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>
#include <vector>

namespace hs2port {

class AudioMidiSettings : public juce::Component, private juce::ChangeListener {
public:
    // `changed` is called after every change (the standalone app saves its device state there).
    AudioMidiSettings(juce::AudioDeviceManager& devices, std::function<void()> changed);
    ~AudioMidiSettings() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void focusFirst();

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void update();
    void applyDevice(int field);  // 0 output device, 1 sample rate, 2 buffer size
    void rebuildMidiInputs();
    static void fill(juce::ComboBox& box, const juce::StringArray& items, int selected);

    juce::AudioDeviceManager& devices_;
    std::function<void()> changed_;
    juce::String error_;

    juce::Label driverLabel_, outputLabel_, rateLabel_, bufferLabel_, midiLabel_, statusLabel_;
    juce::ComboBox driver_, output_, rate_, buffer_;
    std::vector<int> bufferSizes_;
    std::vector<double> rates_;
    juce::TextButton refresh_{"Refresh devices"}, panel_{"Driver control panel..."}, close_{"Close"};
    juce::Component midiGroup_;
    std::vector<std::unique_ptr<juce::ToggleButton>> midiToggles_;
    juce::Array<juce::MidiDeviceInfo> midiDevices_;
    juce::TextEditor status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioMidiSettings)
};

// True when running as the standalone app (its audio device manager is reachable).
bool standaloneAudioAvailable();
// Opens the Audio and MIDI settings window of the standalone app (no-op in a plugin host).
void showAudioMidiSettings(juce::Component* centreAround);

}  // namespace hs2port
