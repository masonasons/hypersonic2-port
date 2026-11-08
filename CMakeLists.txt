cmake_minimum_required(VERSION 3.22)
project(HS2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(core)
add_subdirectory(tools/cli)

# The JUCE plugin (VST3 / AU / Standalone). Off by default so the core and CLI tools build without JUCE.
option(HS2_BUILD_PLUGIN "Build the JUCE plugin" OFF)
if(HS2_BUILD_PLUGIN)
  enable_language(C)
  # JUCE 9.0.5, cloned into the build folder and patched there (plugin/juce_patch/apply.cmake: VST3 program changes
  # per MIDI channel). The clone comes from a local JUCE git checkout when HS2_JUCE_DIR is one (it is never
  # modified), else from GitHub.
  set(HS2_JUCE_DIR "C:/programs/JUCE" CACHE PATH "Local JUCE git checkout to clone from (GitHub when it does not exist)")
  if(EXISTS "${HS2_JUCE_DIR}/.git")
    set(HS2_JUCE_REPO "${HS2_JUCE_DIR}")
  else()
    set(HS2_JUCE_REPO https://github.com/juce-framework/JUCE.git)
  endif()
  include(FetchContent)
  FetchContent_Declare(juce
    GIT_REPOSITORY ${HS2_JUCE_REPO}
    GIT_TAG 9.0.5
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/plugin/juce_patch/apply.cmake)
  FetchContent_MakeAvailable(juce)
  add_subdirectory(plugin)
endif()
