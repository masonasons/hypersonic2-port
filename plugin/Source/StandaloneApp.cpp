#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "ProfileDirectory.h"
#include "AudioSettings.h"

namespace hs2port {
class StandaloneApp final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return JucePlugin_Name; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
        juce::PropertiesFile::Options options;
        options.applicationName = getApplicationName();
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        const auto profile = profileDirectory();
        const auto file = profile == juce::File() ? options.getDefaultFile()
                                                  : profile.getChildFile("HS2 Port.settings");
        settings_ = std::make_unique<juce::PropertiesFile>(file, options);
        const auto background = juce::LookAndFeel::getDefaultLookAndFeel().findColour(
            juce::ResizableWindow::backgroundColourId);
        auto holder = std::make_unique<juce::StandalonePluginHolder>(settings_.get(), false, juce::String{}, nullptr,
                        juce::Array<juce::StandalonePluginHolder::PluginInOuts>{}, false);
#if JUCE_MAC
        setMacVirtualMidiEnabled(settings_->getBoolValue("virtualMidiEnabled", true));
#endif
        window_ = std::make_unique<juce::StandaloneFilterWindow>(getApplicationName(), background, std::move(holder));
        anotherInstanceStarted({});
    }
    void anotherInstanceStarted(const juce::String&) override {
        if (window_ != nullptr) {
            window_->setVisible(true);
            window_->setMinimised(false);
            window_->toFront(true);
        }
    }
    void systemRequestedQuit() override {
        if (window_ != nullptr) window_->getPluginHolder()->savePluginState();
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents()) {
            juce::Timer::callAfterDelay(100, [] {
                if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
            });
            return;
        }
        quit();
    }
    void shutdown() override {
#if JUCE_MAC
        stopMacVirtualMidi();
#endif
        window_.reset();
        if (settings_ != nullptr) settings_->saveIfNeeded();
        settings_.reset();
    }
private:
    std::unique_ptr<juce::PropertiesFile> settings_;
    std::unique_ptr<juce::StandaloneFilterWindow> window_;
};
}

JUCE_CREATE_APPLICATION_DEFINE(hs2port::StandaloneApp)
