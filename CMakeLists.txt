cmake_minimum_required(VERSION 3.22)
project(HS2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The Windows 7 build: the plugin against JUCE 7.0.12 (the last JUCE that runs on Windows 7; JUCE 8 and 9 need
# Windows 10), with the C++ runtime linked in, so a Windows 7 machine needs no Visual C++ redistributable or Universal
# C runtime update. The runtime choice must come before any target (the engine library links into the plugin).
option(HS2_JUCE7 "Build the plugin against JUCE 7.0.12 for Windows 7 and later (the normal build uses JUCE 9.0.5)" OFF)
if(HS2_JUCE7 AND MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_subdirectory(core)
add_subdirectory(tools/cli)

# The JUCE plugin (VST3 / AU / Standalone). Off by default so the core and CLI tools build without JUCE.
option(HS2_BUILD_PLUGIN "Build the JUCE plugin" OFF)
if(HS2_BUILD_PLUGIN)
  enable_language(C)
  # JUCE 9.0.5 (or 7.0.12 with HS2_JUCE7), cloned into the build folder and patched there
  # (plugin/juce_patch/apply.cmake: VST3 program changes per MIDI channel). The clone comes from a local JUCE git
  # checkout when HS2_JUCE_DIR is one (it is never modified), else from GitHub; JUCE 7 always from GitHub.
  set(HS2_JUCE_DIR "C:/programs/JUCE" CACHE PATH "Local JUCE git checkout to clone from (GitHub when it does not exist)")
  if(EXISTS "${HS2_JUCE_DIR}/.git" AND NOT HS2_JUCE7)
    set(HS2_JUCE_REPO "${HS2_JUCE_DIR}")
  else()
    set(HS2_JUCE_REPO https://github.com/juce-framework/JUCE.git)
  endif()
  if(HS2_JUCE7)
    set(hs2_juce_tag 7.0.12)
  else()
    set(hs2_juce_tag 9.0.5)
  endif()
  include(FetchContent)
  FetchContent_Declare(juce
    GIT_REPOSITORY ${HS2_JUCE_REPO}
    GIT_TAG ${hs2_juce_tag}
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/plugin/juce_patch/apply.cmake)
  FetchContent_MakeAvailable(juce)
  add_subdirectory(plugin)
endif()
