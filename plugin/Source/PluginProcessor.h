#pragma once
// HS2 Port: the Hypersonic 2 16-part multitimbral instrument (hs2::Engine). Part N plays MIDI channel N. Patches load
// into single parts; a combi loads a whole 16-part setup with its global effects.
//
// Threading:
//  * Patch and combi files are read and parsed on a background thread (LoaderThread). It also builds a throwaway
//    player for each patch, which loads the sample banks and wavetables into the shared caches, so that the load
//    itself (Engine::loadPatch / loadCombi) is quick. It then queues the load as a Command.
//  * The audio thread applies queued commands at the start of a block (the engine is single-threaded), so the parts
//    that keep playing are never interrupted.
//  * A complete new engine is built on the loader thread only for a state restore, a new content folder or a new
//    sample rate, and swapped in under a SpinLock that the audio thread only try-locks.
//  * Offline renders wait for pending loads, so a render never starts silent or with the wrong patch.
//
// Host parameters mirror the engine's own VST parameters for the master volume and each part's mixer and
// Hyperknobs. Changes go both ways: the host's (automation, the editor) are applied to the engine by the audio
// thread; the engine's (patch loads, combis, MIDI controllers) are pushed to the host parameters on the message
// thread.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "PatchLibrary.h"
#include "hs2/combi.h"
#include "hs2/content.h"
#include "hs2/engine.h"

namespace hs2port {

constexpr int kNumParts = hs2::Engine::kParts;
constexpr int kNumKnobs = 6;
constexpr int kNumSends = 4;

// The host parameters of one part, in order. Engine part parameter j (index part * 0x98 + j) in kPartParamJ.
enum PartParam { ppMute, ppLevel, ppPan, ppSend1, ppKnob1 = ppSend1 + kNumSends, kPartParams = ppKnob1 + kNumKnobs };
constexpr int partParamJ(int pp) {
    return pp == ppMute ? 1 : pp == ppLevel ? 2 : pp == ppPan ? 3 : pp < ppKnob1 ? 5 + (pp - ppSend1) : 9 + (pp - ppKnob1);
}
constexpr int kNumExposed = 1 + kNumParts * kPartParams;  // 0 = master volume
constexpr int exposedIndex(int part, int pp) { return 1 + part * kPartParams + pp; }

// Patch, bank and combi names are Windows-1252 / Latin-1 bytes: converted 1:1 (no UTF-8 decoding).
juce::String fromLatin1(const std::string& s);
std::string toLatin1(const juce::String& s);

// Settings shared by every instance in the process: the content folder and the piano memory mode.
class GlobalSettings {
public:
    GlobalSettings();
    juce::File contentFolder() const;
    void setContentFolder(const juce::File& f);
    int memoryMode() const;  // 0 ECO, 1 MID, 2 XXL
    void setMemoryMode(int m);
    static juce::File defaultContentFolder();

private:
    std::unique_ptr<juce::PropertiesFile> props_;
    mutable std::mutex mu_;
};

// One Content per folder, shared by every instance (sample banks are large). Content and the engine's file caches
// are thread-safe.
class SharedContent {
public:
    std::shared_ptr<hs2::Content> get(const juce::File& folder);

private:
    std::mutex mu_;
    std::map<juce::String, std::weak_ptr<hs2::Content>> map_;
};

// What a part holds, for the editor.
struct PatchInfo {
    bool empty = true;
    int bank = -1, index = -1;     // bank -1: the patch came from a combi
    juce::String name, bankName, description, controllers, author;
    std::array<juce::String, kNumKnobs> knobNames;
    juce::StringArray warnings;
};

// What is loaded, as the state saves it: an optional combi, then per part what was loaded after it.
struct Setup {
    int combiBank = -1, combiIndex = -1;
    juce::String combiName;
    struct Part {
        enum Kind { fromCombi, patch, cleared } kind = fromCombi;
        int bank = -1, index = -1;
        juce::String name;
    };
    std::array<Part, kNumParts> parts;
};

class HS2Processor : public juce::AudioProcessor, public juce::VST3ClientExtensions, private juce::Timer {
public:
    HS2Processor();
    ~HS2Processor() override;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HS2 Port"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // MIDI program changes in VST3: VST3 has no program change event. A host maps a MIDI program change to the
    // plugin's program list parameter, which JUCE turns into setCurrentProgram (message thread, no MIDI channel).
    // So the 128 "programs" are the program numbers: setCurrentProgram(n) becomes a MIDI program change n on the
    // channel of the latest bank select (CC 0 / CC 32, which do arrive as MIDI), part 1 if none came yet, and the
    // engine handles it like any program change. Hosts that send raw MIDI (AU, Standalone) never call this.
    // JUCE calls setCurrentProgram only for a number that differs from getCurrentProgram, so the current program is
    // always the extra entry 128 ("none"): the same number again (e.g. with another bank) still arrives.
    static constexpr int kNoProgram = 128;
    int getNumPrograms() override { return kNoProgram + 1; }
    // With the patched JUCE (plugin/juce_patch): one program list per MIDI channel, so a host's program changes keep
    // their channel and several channels can change at once (a MIDI file with bank / program changes on every
    // channel at the start). The root list above stays for hosts that do not use per-channel units.
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }
    bool getPluginHasProgramChangePerMidiChannel() const override { return true; }
    void setProgramForMidiChannel(int channel, int program) override;
    juce::String getProgramNameForMidiChannel(int channel, int program) override;
    int getCurrentProgram() override { return kNoProgram; }
    void setCurrentProgram(int program) override;
    const juce::String getProgramName(int program) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- used by the editor (message thread) ----
    juce::RangedAudioParameter& exposed(int i) const { return *exposed_[size_t(i)]; }
    juce::RangedAudioParameter& partParam(int part, int pp) const { return exposed(exposedIndex(part, pp)); }
    std::shared_ptr<const PatchLibrary> library() const {
        std::lock_guard<std::mutex> lk(infoMu_);
        return library_;
    }
    bool libraryReady() const { return libraryReady_.load(); }
    PatchInfo partInfo(int part) const;
    juce::String combiName() const;
    juce::String statusText() const;
    bool isLoading() const;
    // Load a patch into a part (its stored Hyperknob values), or a combi (all parts). `explicitRequest`: the user
    // asked for this load in the editor (Enter, a button), so a failure is reported by moving focus to the status.
    void requestPatch(int part, int bank, int index, bool explicitRequest);
    void requestCombi(int bank, int index, bool explicitRequest);
    void requestClearPart(int part);
    juce::File contentFolder() const { return settings_->contentFolder(); }
    void setContentFolder(const juce::File& f);
    int memoryMode() const { return settings_->memoryMode(); }
    void setMemoryMode(int m);
    bool midiProgramChanges() const { return midiProgramChanges_.load(); }
    void setMidiProgramChanges(bool on) { midiProgramChanges_ = on; }
    // Effect tails ring out (not in the original, which cuts some delay tails when the last note ends). Default on.
    bool ringOut() const { return ringOut_.load(); }
    void setRingOut(bool on) { ringOut_ = on; }
    // Writes VST3 (and on the Mac AU) presets and Komplete Kontrol NKS presets for every loadable patch and combi of
    // the content folder (PresetExport.h), on a background thread; progress and the result go to the status text.
    void installPresets();
    bool installingPresets() const { return presetJob_ != nullptr && presetJob_->isThreadRunning(); }
    // Loading a patch stops the part's previous patch (short fade) instead of letting it sound on. Default on.
    bool cutOnLoad() const { return cutOnLoad_.load(); }
    void setCutOnLoad(bool on) { cutOnLoad_ = on; }
    // "Hyperknob 1: Thump" (the loaded patch's own name for the knob, if it has one).
    juce::String knobName(int part, int k) const;
    juce::String patchKnobName(int part, int k) const;  // the patch's own name, may be empty
    juce::String partSummary(int part) const;  // "Part 3: Analogy" / "Part 3: empty"

    // The part the editor works on (kept with the state, so the editor reopens on it).
    std::atomic<int> selectedPart{0};
    // Bumped on every change the editor should pick up (load finished, library scanned, status changed).
    std::atomic<int> changeCounter{0};
    // Set when an explicitly requested load failed; the editor reads and clears it. (No spoken announcements:
    // JUCE's postAnnouncement uses a separate SAPI voice on Windows, not the user's screen reader.)
    std::atomic<bool> explicitLoadFailed{false};

private:
    struct Active {
        std::shared_ptr<hs2::Content> content;  // must outlive the engine
        std::unique_ptr<hs2::Engine> engine;
        double sampleRate = 0;
    };
    // A load prepared by the loader thread, applied by the audio thread between blocks.
    struct Command {
        enum Kind { loadPatch, loadCombi, clearPart, memoryMode } kind = loadPatch;
        int part = 0, bank = -1;
        float value = 0;
        std::shared_ptr<const hs2::Patch> patch;
        std::shared_ptr<const hs2::Combi> combi;
    };
    // A load the user or MIDI asked for, handled by the loader thread.
    struct Request {
        enum Kind { patch, combi, clear, memory } kind = patch;
        int part = 0, bank = -1, index = -1;
        juce::String name;  // to find the patch again if the bank changed
        bool explicitRequest = false;
    };
    // A complete rebuild (state restore, content folder, sample rate).
    struct Rebuild {
        Setup setup;
        std::array<float, kNumExposed> values{};
        std::array<bool, kNumExposed> valid{};
        bool explicitRequest = false;
    };
    class LoaderThread;
    friend class LoaderThread;
    class EngineParameter;

    void timerCallback() override;  // pushes engine-side parameter changes to the host parameters
    void handleRequest(const Request& r);
    void rebuildNow(const Rebuild& r);  // loader thread / prepareToPlay
    bool prepareCommand(const Request& r, hs2::Content& content, Command& cmd, PatchInfo* info,
                        std::array<PatchInfo, kNumParts>* combiInfos, juce::String& error);
    Rebuild currentRebuild() const;     // the current setup and parameter values
    std::array<float, kNumExposed> currentValues(std::array<bool, kNumExposed>* valid) const;
    void rescanLibrary();
    void setStatus(const juce::String& s);
    void submit(const Request& r);
    void submitRebuild(const Rebuild& r);
    void applyCommand(hs2::Engine& e, const Command& c);
    int engineIndex(int i) const { return i == 0 ? 0 : ((i - 1) / kPartParams) * hs2::Engine::kParamsPerPart +
                                                           partParamJ((i - 1) % kPartParams); }
    void syncParameters(hs2::Engine& e);  // audio thread

    juce::SharedResourcePointer<GlobalSettings> settings_;
    juce::SharedResourcePointer<SharedContent> sharedContent_;
    std::array<juce::RangedAudioParameter*, kNumExposed> exposed_{};

    // Parameter sync (per exposed parameter).
    std::array<std::atomic<float>, kNumExposed> engineVal_;  // the engine's value as last seen (audio / rebuild)
    std::array<std::atomic<float>, kNumExposed> hostSeen_;   // the host value the audio thread last handled
    std::array<std::atomic<float>, kNumExposed> echo_;       // a value the message thread pushed to the host
    std::array<std::atomic<bool>, kNumExposed> dirty_;       // engine changed: push engineVal_ to the host
    std::atomic<bool> anyDirty_{false};
    std::atomic<bool> resyncAll_{false};
    std::atomic<int> infoGeneration_{0};

    // Audio-thread state.
    juce::SpinLock swapLock_;
    std::unique_ptr<Active> active_;        // guarded by swapLock_
    juce::SpinLock cmdLock_;
    std::vector<Command> cmdQueue_;         // guarded by cmdLock_ (loader -> audio)
    std::vector<Command> cmdWork_;          // audio thread
    std::vector<float> interleaved_;
    std::vector<hs2::MidiEvent> events_;
    std::atomic<double> sampleRate_{44100.0};
    // MIDI program change requests (audio -> loader).
    juce::AbstractFifo midiFifo_{64};
    std::array<hs2::Engine::ProgramRequest, 64> midiReqs_{};
    std::atomic<bool> midiProgramChanges_{true};
    std::atomic<bool> ringOut_{true};
    std::atomic<bool> cutOnLoad_{true};
    // VST3 program changes (setCurrentProgram -> audio thread, see getNumPrograms).
    std::atomic<int> pendingProgram_{-1};
    std::array<std::atomic<int>, 16> pendingChannelProgram_;  // per MIDI channel, -1 = none (patched JUCE)
    // Controllers that come with a program change (audio thread). The part's CC map sends CC7 / CC10 / CC91.. to the
    // part's own Level / Pan / sends, so they are lost when the new patch replaces the part a block or more later
    // (the original's loader runs even later, from the host's idle calls). A MIDI file sets volume, pan and sends
    // at the moment of its program changes, and a VST3 host delivers the program change later than the controllers
    // (through the program list, on another thread). So when a new patch is put in place, the controllers its
    // channel received in the last kCcRecentSeconds are sent to it again.
    static constexpr double kCcRecentSeconds = 3.0;
    std::array<std::array<int8_t, 128>, 16> ccLast_{};   // last value per channel / CC, -1 = none
    std::array<std::array<int64_t, 128>, 16> ccTime_{};  // when it came (clock_)
    int64_t clock_ = 0;                                  // frames processed (audio thread)
    std::atomic<int> bankChannel_{0};                  // MIDI channel (0-based) of the latest bank select
    std::array<std::atomic<int>, 16> bankSelect_{};    // per channel: CC0 * 128 + CC32 as last received
    std::array<bool, 16> sawBankLsb_{};                // audio thread: a CC32 came on this channel (else CC0 = bank)

    // Loader state.
    std::unique_ptr<LoaderThread> loader_;
    std::unique_ptr<juce::Thread> presetJob_;  // installPresets
    std::atomic<bool> presetCancel_{false};
    std::mutex requestMu_;
    std::vector<Request> requests_;         // guarded by requestMu_
    std::unique_ptr<Rebuild> rebuild_;      // guarded by requestMu_
    std::atomic<bool> requestPending_{false};
    std::atomic<bool> loadBusy_{false};
    std::atomic<bool> rescanRequested_{false};
    std::mutex loaderMu_;                   // the loader's work runs on the loader thread and from prepareToPlay

    // Editor-visible state (guarded by infoMu_).
    mutable std::mutex infoMu_;
    Setup setup_;
    std::array<PatchInfo, kNumParts> infos_;
    juce::String combiName_;
    juce::String status_;
    std::shared_ptr<const PatchLibrary> library_;
    std::atomic<bool> libraryReady_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HS2Processor)
};

}  // namespace hs2port
