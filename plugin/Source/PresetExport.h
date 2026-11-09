#pragma once
// Preset files for the user's own Hypersonic 2 content: VST3 presets (.vstpreset), Audio Unit presets (.aupreset,
// Mac) and Komplete Kontrol NKS presets (.nksf). A preset holds the plugin state with the patch (or combi) reference
// only: bank, number and name. Nothing from Steinberg is stored; the plugin reads the patch from the content folder.
// Files are generated on the user's machine from the content found there (copy-protected patches are skipped).
//
// Formats:
//  * .vstpreset: VST3 SDK preset file ("VST3", version 1, the component class ID as 32 ASCII hex digits, list offset;
//    a "Comp" chunk with the component state = the plugin's getStateInformation data; the chunk list).
//  * .aupreset: an XML property list with the AU identity (type 'aumu', subtype, manufacturer, version 0, name) and
//    the plugin state under "jucePluginState" (JUCE's AU wrapper reads it from there).
//  * .nksf: a RIFF file of form type "NIKS" with the chunks NISI (preset metadata), NICA (knob pages), PLID (the
//    plugin: its VST3 class ID as four 32-bit numbers) and PCHK (a complete .vstpreset); each chunk starts with the
//    32-bit version 1, the first three are MessagePack maps.
//
// Re-running replaces only the files this exporter wrote before (a receipt in each group folder lists them); presets
// the user saved there are kept.
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>

#include "PatchLibrary.h"

namespace hs2port {

struct PresetExportOptions {
    juce::File contentRoot;  // the Hypersonic 2 content folder (patch banks are read for knob names and descriptions)
    juce::File vst3Root;     // .vstpreset files (empty: none)
    juce::File auRoot;       // .aupreset files (empty: none)
    juce::File nksRoot;      // .nksf files (empty: none)
};

struct PresetExportResult {
    int patches = 0, combis = 0;   // presets written per format
    int skippedProtected = 0;      // copy-protected patches (not exported)
    juce::StringArray errors;
};

// Writes the presets for every loadable patch and combi in `library`. `progress(done, total)` is called now and then;
// `cancel` stops it between presets. Not for the audio thread.
PresetExportResult exportPresets(const PatchLibrary& library, const PresetExportOptions& options,
                                 const std::function<void(int, int)>& progress = {},
                                 const std::atomic<bool>* cancel = nullptr);

// The standard per-user preset folders for this plugin on this platform.
juce::File defaultVst3PresetFolder();
juce::File defaultAuPresetFolder();  // Mac only (empty elsewhere)
juce::File defaultNksPresetFolder();

// The plugin's preset state for one patch in part 1, or for a combi (the state format of HS2Processor; the
// "preset" attribute tells setStateInformation to keep the user's settings).
juce::MemoryBlock patchPresetState(const PatchRef& ref);

// The file formats (exposed for tests).
juce::MemoryBlock makeVstPreset(const juce::MemoryBlock& componentState);
juce::String makeAuPreset(const juce::MemoryBlock& state, const juce::String& name);

}  // namespace hs2port
