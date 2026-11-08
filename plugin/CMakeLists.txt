# Hypersonic 2 port as a JUCE plugin. Configure the top level with -DHS2_BUILD_PLUGIN=ON (MSVC on Windows,
# Xcode / Apple Clang on Mac). The engine (hs2core) is the same library the CLI tools and oracle tests use.
juce_add_plugin(HS2Port
    VERSION 0.1.0
    COMPANY_NAME "HS2 Port"
    BUNDLE_ID com.hs2port.HS2Port
    PLUGIN_MANUFACTURER_CODE Hs2p
    PLUGIN_CODE Hs2P
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "HS2 Port"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(HS2Port PRIVATE
    Source/PatchLibrary.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(HS2Port PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(HS2Port
    PRIVATE
        hs2core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Test host: loads the built VST3 like a DAW and renders offline (see tests/hs2plughost.cpp).
juce_add_console_app(hs2plughost VERSION 0.1.0 PRODUCT_NAME "hs2plughost")
target_sources(hs2plughost PRIVATE tests/hs2plughost.cpp)
target_compile_definitions(hs2plughost PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(hs2plughost PRIVATE
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)
