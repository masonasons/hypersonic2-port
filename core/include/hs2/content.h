#pragma once
// The user's installed "Hypersonic 2 Content" folder. Banks are addressed by their NNN file-name prefix,
// which is also the number patches use (sample element Bank = trunc(v*128.9)) and MIDI bank select uses.
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "hs2/samples.h"

namespace hs2 {

class Content {
public:
    explicit Content(std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path patchBankPath(int number) const;  // empty if absent
    // Loaded on first use; nullptr if absent. Thread-safe (the plugin shares one Content between instances and
    // their loader threads); the file is read outside the lock, so other banks stay reachable meanwhile.
    SampleBank* sampleBank(int number);
    // Decoded sample bytes held by every open sample bank.
    size_t decodedSampleBytes();
    // SampleBank::release for every open bank (same contract: nothing may read a dropped sample).
    size_t releaseSamples(const std::function<bool(const SampleBank*, uint32_t, const SampleData*)>& keep,
                          std::vector<std::unique_ptr<SampleData>>& out);

private:
    std::filesystem::path root_;
    std::map<int, std::filesystem::path> patchFiles_, sampleFiles_;
    std::map<int, std::unique_ptr<SampleBank>> sampleBanks_;
    std::mutex mu_;
};

}  // namespace hs2
