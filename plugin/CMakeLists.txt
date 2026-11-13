# Hypersonic 2 port as a JUCE plugin. Configure the top level with -DHS2_BUILD_PLUGIN=ON (MSVC on Windows,
# Xcode / Apple Clang on Mac). The engine (hs2core) is the same library the CLI tools and oracle tests use.
juce_add_plugin(HS2Port
    VERSION 0.6.0
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

target_sources(HS2Port_Standalone PRIVATE Source/StandaloneApp.cpp)
target_compile_definitions(HS2Port_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)

target_sources(HS2Port PRIVATE
    Source/AudioSettings.cpp
    Source/PatchEdit.cpp
    Source/PatchLibrary.cpp
    Source/PresetExport.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

if(APPLE)
  target_sources(HS2Port PRIVATE Source/MacMenuBar.mm)
endif()
if(APPLE OR WIN32)
  juce_add_console_app(hs2_midi_input_preferences VERSION 0.1.0 PRODUCT_NAME "HS2 MIDI Input Preferences Test")
  target_sources(hs2_midi_input_preferences PRIVATE tests/midi_input_preferences.cpp)
  target_link_libraries(hs2_midi_input_preferences PRIVATE juce::juce_audio_devices juce::juce_events
    juce::juce_recommended_config_flags)
endif()

# Legacy parameter ids: a host parameter's VST3 / AU id is its index (0 master, then 13 per part). Komplete Kontrol's
# knob pages in the NKS presets (PresetExport.cpp) name parameters by that number.
target_compile_definitions(HS2Port PUBLIC
    JUCE_FORCE_USE_LEGACY_PARAM_IDS=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# ASIO for the standalone app on Windows: needs Steinberg's ASIO SDK (the folder that contains common/asio.h). It is
# not part of this repository. HS2_ASIO_SDK_DIR points at a local copy; when there is none, CMake downloads ASIO SDK
# 2.3.4 from Steinberg during configure (pinned by its SHA-256), as it does with JUCE. HS2_REQUIRE_ASIO=ON (the
# Windows dist script, CI) stops the configure when ASIO cannot be enabled, so a release never loses it silently.
# The SDK is licensed under Steinberg's ASIO licence or GPLv3.
if(WIN32)
  target_sources(HS2Port PRIVATE Source/TypingAnnouncementWindows.cpp)
  target_link_libraries(HS2Port PRIVATE oleaut32)
  option(HS2_ASIO "ASIO in the standalone app (the ASIO SDK is downloaded when HS2_ASIO_SDK_DIR has none)" ON)
  option(HS2_REQUIRE_ASIO "Stop the configure when ASIO cannot be enabled" OFF)
  set(HS2_ASIO_SDK_DIR "C:/programs/asiosdk/ASIOSDK" CACHE PATH "Steinberg ASIO SDK folder (contains common/asio.h)")
  set(hs2_asio_dir "")
  if(HS2_ASIO)
    if(EXISTS "${HS2_ASIO_SDK_DIR}/common/asio.h")
      set(hs2_asio_dir "${HS2_ASIO_SDK_DIR}")
    else()
      include(FetchContent)
      FetchContent_Declare(asiosdk
        URL https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
        URL_HASH SHA256=D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_MakeAvailable(asiosdk)  # no CMakeLists.txt in the SDK: download and unpack only
      if(EXISTS "${asiosdk_SOURCE_DIR}/common/asio.h")
        set(hs2_asio_dir "${asiosdk_SOURCE_DIR}")
      endif()
    endif()
  endif()
  if(hs2_asio_dir)
    target_compile_definitions(HS2Port PUBLIC JUCE_ASIO=1)
    target_include_directories(HS2Port PRIVATE "${hs2_asio_dir}/common")
    message(STATUS "HS2: ASIO enabled (${hs2_asio_dir})")
  elseif(HS2_REQUIRE_ASIO)
    message(FATAL_ERROR "HS2: ASIO is required (HS2_REQUIRE_ASIO) but the ASIO SDK is not available")
  else()
    message(STATUS "HS2: ASIO off; the standalone app has the Windows Audio drivers only")
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

juce_add_console_app(hs2_preset_transaction VERSION 0.1.0 PRODUCT_NAME "hs2_preset_transaction")
target_sources(hs2_preset_transaction PRIVATE tests/preset_transaction.cpp Source/PresetExport.cpp Source/PatchLibrary.cpp)
target_compile_definitions(hs2_preset_transaction PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_FORCE_USE_LEGACY_PARAM_IDS=1)
target_link_libraries(hs2_preset_transaction PRIVATE hs2core juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_recommended_config_flags)

juce_add_console_app(hs2_keyboard_controls VERSION 0.1.0 PRODUCT_NAME "HS2 Keyboard Controls Test")
target_sources(hs2_keyboard_controls PRIVATE tests/keyboard_controls.cpp)
target_compile_definitions(hs2_keyboard_controls PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(hs2_keyboard_controls PRIVATE juce::juce_gui_basics juce::juce_recommended_config_flags)
