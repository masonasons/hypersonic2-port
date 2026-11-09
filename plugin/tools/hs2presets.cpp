// hs2presets: writes the HS2 Port preset files for a Hypersonic 2 content folder (the same code as the plugin's
// "Install presets..." button, PresetExport.h). Without --vst3 / --au / --nks it writes nothing.
//   hs2presets --content "C:/Hypersonic 2 Content" [--vst3 DIR] [--au DIR] [--nks DIR] [--defaults]
// --defaults uses the standard per-user folders of this platform (as the plugin does; NKS only with Komplete Kontrol
// installed).
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

#include "../Source/PatchLibrary.h"
#include "../Source/PresetExport.h"

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI init;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(juce::String::fromUTF8(argv[i]));
    auto opt = [&](const char* name) {
        const int i = args.indexOf(name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
    };
    const auto content = opt("--content");
    if (content.isEmpty()) {
        std::puts("usage: hs2presets --content <Hypersonic 2 Content> [--vst3 DIR] [--au DIR] [--nks DIR] [--defaults]");
        return 2;
    }
    hs2port::PresetExportOptions o;
    o.contentRoot = juce::File(content);
    if (args.contains("--defaults")) {
        o.vst3Root = hs2port::defaultVst3PresetFolder();
        o.auRoot = hs2port::defaultAuPresetFolder();
        if (hs2port::kompleteKontrolInstalled()) o.nksRoot = hs2port::defaultNksPresetFolder();
    }
    if (opt("--vst3").isNotEmpty()) o.vst3Root = juce::File(opt("--vst3"));
    if (opt("--au").isNotEmpty()) o.auRoot = juce::File(opt("--au"));
    if (opt("--nks").isNotEmpty()) o.nksRoot = juce::File(opt("--nks"));

    const auto lib = hs2port::PatchLibrary::scan(std::filesystem::path(o.contentRoot.getFullPathName().toWideCharPointer()));
    for (const auto& e : lib.errors) std::fprintf(stderr, "library: %s\n", e.c_str());
    const auto r = hs2port::exportPresets(lib, o);
    std::printf("patches %d, combis %d, copy-protected skipped %d\n", r.patches, r.combis, r.skippedProtected);
    for (const auto& e : r.errors) std::fprintf(stderr, "error: %s\n", e.toRawUTF8());
    return r.errors.isEmpty() ? 0 : 1;
}
