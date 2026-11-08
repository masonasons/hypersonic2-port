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
  # A local JUCE checkout if there is one (HS2_JUCE_DIR), else JUCE 9.0.5 is downloaded at configure time.
  set(HS2_JUCE_DIR "C:/programs/JUCE" CACHE PATH "JUCE source folder (downloaded when it does not exist)")
  if(EXISTS "${HS2_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${HS2_JUCE_DIR} juce)
  else()
    include(FetchContent)
    FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.5 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
  endif()
  add_subdirectory(plugin)
endif()
