#pragma once
#include <juce_core/juce_core.h>

namespace hs2port {
inline juce::File profileDirectory() {
    const auto path = juce::SystemStats::getEnvironmentVariable("HS2_PROFILE_DIR", {});
    return juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File();
}
}
