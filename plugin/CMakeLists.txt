# Hypersonic 2 port as a JUCE plugin. Configure the top level with -DHS2_BUILD_PLUGIN=ON (MSVC on Windows,
# Xcode / Apple Clang on Mac). The engine (hs2core) is the same library the CLI tools and oracle tests use.
juce_add_plugin(HS2Port
    VERSION 0.3.0
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
    Source/AudioSettings.cpp
    Source/PatchLibrary.cpp
    Source/PresetExport.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

# Legacy parameter ids: a host parameter's VST3 / AU id is its index (0 master, then 13 per part). Komplete Kontrol's
# knob pages in the NKS presets (PresetExport.cpp) name parameters by that number.
target_compile_definitions(HS2Port PUBLIC
    JUCE_FORCE_USE_LEGACY_PARAM_IDS=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# ASIO for the standalone app on Windows: needs Steinberg's ASIO SDK (https://www.steinberg.net/asiosdk, the folder
# that holds common/asio.h). It is not part of this repository; without it the app has the Windows Audio drivers only.
# The SDK is licensed under Steinberg's ASIO licence or GPLv3.
if(WIN32)
  set(HS2_ASIO_SDK_DIR "C:/programs/asiosdk/ASIOSDK" CACHE PATH "Steinberg ASIO SDK folder (contains common/asio.h)")
  if(EXISTS "${HS2_ASIO_SDK_DIR}/common/asio.h")
    target_compile_definitions(HS2Port PUBLIC JUCE_ASIO=1)
    target_include_directories(HS2Port PRIVATE "${HS2_ASIO_SDK_DIR}/common")
    message(STATUS "HS2: ASIO enabled (${HS2_ASIO_SDK_DIR})")
  else()
    message(STATUS "HS2: no ASIO SDK at HS2_ASIO_SDK_DIR (${HS2_ASIO_SDK_DIR}); the standalone app has no ASIO")
  endif()
endif()

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

# hs2presets: writes the preset files (PresetExport) without the plugin; for tests and scripts.
#   hs2presets --content "C:/Hypersonic 2 Content" [--vst3 DIR] [--au DIR] [--nks DIR] [--no-previews]
juce_add_console_app(hs2presets VERSION 0.1.0 PRODUCT_NAME "hs2presets")
target_sources(hs2presets PRIVATE tools/hs2presets.cpp Source/PresetExport.cpp Source/PatchLibrary.cpp)
target_compile_definitions(hs2presets PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_FORCE_USE_LEGACY_PARAM_IDS=1)
target_link_libraries(hs2presets PRIVATE hs2core juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_recommended_config_flags)
