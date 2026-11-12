#include "hs2/content.h"

#include <cctype>

namespace hs2 {

namespace {
void scan(const std::filesystem::path& dir, std::map<int, std::filesystem::path>& out) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().has_extension()) continue;
        auto name = e.path().filename().string();
        if (name.size() < 4 || !std::isdigit(uint8_t(name[0])) || !std::isdigit(uint8_t(name[1])) ||
            !std::isdigit(uint8_t(name[2])) || name[3] != ' ')
            continue;
        out[std::stoi(name.substr(0, 3))] = e.path();
    }
}
}  // namespace

Content::Content(std::filesystem::path root) : root_(std::move(root)) {
    scan(root_ / "Patches", patchFiles_);
    scan(root_ / "Samples", sampleFiles_);
}

std::filesystem::path Content::patchBankPath(int number) const {
    auto it = patchFiles_.find(number);
    return it == patchFiles_.end() ? std::filesystem::path{} : it->second;
}

SampleBank* Content::sampleBank(int number) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sampleBanks_.find(number);
        if (it != sampleBanks_.end()) return it->second.get();
    }
    std::unique_ptr<SampleBank> sb;
    auto f = sampleFiles_.find(number);
    if (f != sampleFiles_.end()) sb = SampleBank::open(f->second);
    std::lock_guard<std::mutex> lk(mu_);
    auto& slot = sampleBanks_[number];
    if (!slot) slot = std::move(sb);  // another thread may have loaded it meanwhile; keep the first (pointers stay valid)
    return slot.get();
}

size_t Content::decodedSampleBytes() {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& [k, sb] : sampleBanks_)
        if (sb) n += sb->decodedBytes();
    return n;
}

size_t Content::releaseSamples(const std::function<bool(const SampleBank*, uint32_t, const SampleData*)>& keep,
                               std::vector<std::unique_ptr<SampleData>>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& [k, sb] : sampleBanks_)
        if (sb) n += sb->release([&](uint32_t i, const SampleData* sd) { return keep(sb.get(), i, sd); }, out);
    return n;
}

}  // namespace hs2
