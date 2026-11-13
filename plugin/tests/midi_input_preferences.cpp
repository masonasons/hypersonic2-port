#include <juce_audio_devices/juce_audio_devices.h>
#include <iostream>
#if JUCE_MAC
#include <CoreMIDI/CoreMIDI.h>
#endif

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
#if JUCE_MAC
    juce::ignoreUnused(argc, argv);
    const auto name = "HS2 MIDI preferences test " + juce::Uuid().toString();
    struct Source {
        MIDIClientRef client{};
        MIDIEndpointRef endpoint{};
        ~Source() { if (endpoint) MIDIEndpointDispose(endpoint); if (client) MIDIClientDispose(client); }
    } source;
    const auto cfName = CFStringCreateWithCString(nullptr, name.toRawUTF8(), kCFStringEncodingUTF8);
    const auto result = MIDIClientCreate(cfName, nullptr, nullptr, &source.client);
    const auto sourceResult = result == noErr ? MIDISourceCreate(source.client, cfName, &source.endpoint) : result;
    CFRelease(cfName);
    if (sourceResult != noErr) return 1;
    juce::String identifier;
    for (int i = 0; i < 20 && identifier.isEmpty(); ++i) {
        for (const auto& info : juce::MidiInput::getAvailableDevices())
            if (info.name.contains(name)) identifier = info.identifier;
        if (identifier.isEmpty()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }
    if (identifier.isEmpty()) return 2;
    const auto displayName = [&] {
        for (const auto& info : juce::MidiInput::getAvailableDevices())
            if (info.identifier == identifier) return info.name;
        return name;
    }();
#else
    const auto inputs = juce::MidiInput::getAvailableDevices();
    if (argc != 2) {
        for (const auto& info : inputs) std::cout << info.name << '\n';
        return 2;
    }
    const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto& info) { return info.name == argv[1]; });
    if (found == inputs.end()) return 2;
    const auto displayName = found->name;
    const auto identifier = found->identifier;
#endif
    const bool stale = juce::SystemStats::getEnvironmentVariable("HS2_MIDI_TEST_CURRENT_ID", {}).isEmpty();
    juce::XmlElement settings("DEVICESETUP");
    auto* old = settings.createNewChildElement("MIDIINPUT");
    old->setAttribute("name", displayName);
    old->setAttribute("identifier", stale ? juce::String("old-identifier") : identifier);
    auto* offline = settings.createNewChildElement("MIDIINPUT");
    offline->setAttribute("name", "HS2 unrelated disconnected MIDI device");
    offline->setAttribute("identifier", "unrelated-identifier");
    juce::AudioDeviceManager first;
    first.initialise(0, 0, &settings, true);
    if (!first.isMidiInputDeviceEnabled(identifier)) return 3;
    first.setMidiInputDeviceEnabled(identifier, false);
    auto saved = first.createStateXml();
    first.closeAudioDevice();
    if (saved == nullptr || saved->getNumChildElements() != 1
        || saved->getFirstChildElement()->getStringAttribute("identifier") != "unrelated-identifier") {
        std::cerr << "Disabling a re-identified MIDI input left it in the saved preferences.\n";
        return 4;
    }
    juce::AudioDeviceManager second;
    second.initialise(0, 0, saved.get(), true);
    if (second.isMidiInputDeviceEnabled(identifier)) return 5;
    second.setMidiInputDeviceEnabled(identifier, true);
    auto reenabled = second.createStateXml();
    if (reenabled == nullptr || reenabled->getNumChildElements() != 2) return 6;
    second.setMidiInputDeviceEnabled(identifier, false);
    juce::AudioDeviceManager third;
    third.initialise(0, 0, reenabled.get(), true);
    if (!third.isMidiInputDeviceEnabled(identifier)) return 7;
    std::cout << "PASS: disabled MIDI stays off after reload, re-enable persists, unrelated offline input retained.\n";
    return 0;
}
