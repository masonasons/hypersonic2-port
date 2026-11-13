#include "../Source/PresetExport.h"

#include <iostream>
#include <map>
#include <stdexcept>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace {
using namespace hs2port;
using Files = std::map<juce::String, juce::MemoryBlock>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Files snapshot(const juce::File& root) {
    Files result;
    for (const auto& file : root.findChildFiles(juce::File::findFiles, true)) {
        juce::MemoryBlock data;
        require(file.loadFileAsData(data), "cannot read snapshot");
        result.emplace(file.getRelativePathFrom(root), std::move(data));
    }
    return result;
}

PatchLibrary library(int count) {
    PatchLibrary lib;
    for (int i = 0; i < count; ++i) lib.patches.push_back({0, i, "Patch " + std::to_string(i)});
    return lib;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "specify an empty, disposable test folder");
        const juce::File root(juce::String::fromUTF8(argv[1]));
        require(!root.exists() && root.createDirectory().wasOk(), "scratch folder must not already exist");
        hs2port::PresetExportOptions options;
        options.contentRoot = root;
        options.vst3Root = options.auRoot = root.getChildFile("Shared");
        options.nksRoot = root.getChildFile("NKS");
        options.nksPreviews = false;
        require(!hs2port::hasOlderNksPresets(options.nksRoot), "missing library triggered an upgrade");

        const auto old = library(2);
        auto next = library(53);
        next.patches[0].index = 100;  // same name, different state
        next.patches[1].name = "Renamed";
        require(hs2port::exportPresets(old, options).errors.isEmpty(), "seed failed");
        require(!hs2port::hasOlderNksPresets(options.nksRoot), "current library triggered an upgrade");
        const auto oldReceipt = options.nksRoot.getChildFile(".hs2port-nksf-presets.json");
        const auto legacy = options.nksRoot.getChildFile(".hs2port-presets.json");
        auto receiptData = juce::JSON::parse(oldReceipt);
        require(receiptData.getDynamicObject() != nullptr, "cannot read seed receipt");
        receiptData.getDynamicObject()->removeProperty("mappingRevision");
        require(legacy.replaceWithText(juce::JSON::toString(receiptData)) && oldReceipt.deleteFile(),
                "could not seed a legacy receipt");
        require(hs2port::hasOlderNksPresets(options.nksRoot), "legacy installation did not trigger an upgrade");
        const auto custom = options.nksRoot.getChildFile("Patches/0/Patch 2.nksf");
        require(custom.replaceWithText("custom"), "cannot seed a user preset");
        const auto before = snapshot(root);

        std::atomic<bool> cancel{false};
        const auto cancelled = hs2port::exportPresets(next, options, [&](int done, int) {
            if (done == 50) cancel = true;
        }, &cancel);
        require(!cancelled.errors.isEmpty() && snapshot(root) == before,
                "cancelling a partial export changed the existing library");
        cancel = false;
        const auto lateCancel = hs2port::exportPresets(next, options, [&](int done, int total) {
            if (done == total) cancel = true;
        }, &cancel);
        require(!lateCancel.errors.isEmpty() && snapshot(root) == before,
                "cancelling just before replacement changed the existing library");

        auto bad = next;
        bad.patches.push_back({120, 0, "Missing combi", true});
        require(!hs2port::exportPresets(bad, options).errors.isEmpty() && snapshot(root) == before,
                "generation failure changed the existing library");

        const auto missingRoot = root.getChildFile("Offline");
        auto offline = options;
        offline.nksRoot = missingRoot;
        offline.requiredExistingRoots.push_back(missingRoot);
        require(!hs2port::exportPresets(next, offline).errors.isEmpty() && !missingRoot.exists()
                    && snapshot(root) == before,
                "unavailable custom destination was recreated or changed another bank");

        const auto interrupted = options.nksRoot.getChildFile(".hs2port-install-test/replacement-started");
        require(interrupted.getParentDirectory().createDirectory().wasOk() && interrupted.replaceWithText("recover"),
                "cannot seed a recovery marker");
        const auto recoverySnapshot = snapshot(root);
        require(!hs2port::exportPresets(next, options).errors.isEmpty() && snapshot(root) == recoverySnapshot,
                "reinstall overwrote an interrupted transaction");
        require(interrupted.deleteFile() && interrupted.getParentDirectory().deleteFile(),
                "cannot remove the recovery fixture");

#if JUCE_WINDOWS
        const auto held = options.nksRoot.getChildFile("Patches/0/Patch 0.nksf");
        const auto handle = CreateFileW(held.getFullPathName().toWideCharPointer(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "could not lock an old NKS file");
        const auto failed = hs2port::exportPresets(next, options);
        CloseHandle(handle);
        require(!failed.errors.isEmpty() && snapshot(root) == before,
                "commit failure did not restore earlier destinations and receipts");
#endif

        require(hs2port::exportPresets(next, options).errors.isEmpty(), "reinstall failed");
        require(custom.loadFileAsString() == "custom", "reinstall overwrote a user preset");
        require(!options.nksRoot.getChildFile("Patches/0/Patch 1.nksf").exists(), "obsolete preset remains");
        require(options.nksRoot.getChildFile("Patches/0/Patch 2 (2).nksf").exists(), "collision not resolved");
        require(!hs2port::hasOlderNksPresets(options.nksRoot), "updated library still triggers an upgrade");
        std::cout << "Preset cancellation, failure and replacement checks passed.\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
