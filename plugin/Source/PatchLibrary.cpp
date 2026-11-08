#include "PatchLibrary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

#include "hs2/bank.h"

namespace hs2port {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
bool numbered(const std::string& name) {  // "NNN name"
    return name.size() >= 4 && std::isdigit(uint8_t(name[0])) && std::isdigit(uint8_t(name[1])) &&
           std::isdigit(uint8_t(name[2])) && name[3] == ' ';
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}
}  // namespace

PatchLibrary PatchLibrary::scan(const std::filesystem::path& root) {
    PatchLibrary lib;
    std::map<int, std::filesystem::path> files, combiDirs;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root / "Patches", ec)) {
        const auto name = e.path().filename().string();
        if (!numbered(name)) continue;
        const int number = std::stoi(name.substr(0, 3));
        if (e.is_directory()) {
            if (number >= 120) combiDirs[number] = e.path();  // combi banks 120..128
        } else if (e.is_regular_file() && !e.path().has_extension()) {
            files[number] = e.path();
        }
    }
    if (ec) lib.errors.push_back("Cannot read the Patches folder: " + ec.message());

    for (const auto& [number, path] : files) {
        BankInfo b;
        b.number = number;
        b.name = path.filename().string();
        try {
            auto mb = hs2::MilanBank::open(path);
            if (mb->kind != 1) continue;  // not a patch bank
            for (size_t i = 0; i < mb->entries.size(); ++i) {
                const auto& entry = mb->entries[i];
                if (entry.slot1.size == 0) continue;  // empty slot
                PatchRef r;
                r.bank = number;
                r.index = int(i);  // position in the directory, as hs2render's --patch and MIDI program numbers
                r.name = displayName(entry.name);
                if (r.name.empty()) r.name = "Patch " + std::to_string(i + 1);
                b.patches.push_back(int(lib.patches.size()));
                lib.patches.push_back(std::move(r));
            }
        } catch (const std::exception& ex) {
            lib.errors.push_back("Bank " + b.name + ": " + ex.what());
            continue;
        }
        if (!b.patches.empty()) lib.banks.push_back(std::move(b));
    }

    // Combis: "NNN name.fxb" files; the number is the combi's index (MIDI program number on combi bank select).
    for (const auto& [number, dir] : combiDirs) {
        BankInfo b;
        b.number = number;
        b.name = dir.filename().string();
        b.combi = true;
        std::map<int, std::string> found;
        std::error_code ec2;
        for (const auto& f : std::filesystem::directory_iterator(dir, ec2)) {
            const auto fn = f.path().filename().string();
            if (!f.is_regular_file() || f.path().extension() != ".fxb" || fn.size() < 5 || !std::isdigit(uint8_t(fn[0])))
                continue;
            const int idx = std::atoi(fn.c_str());
            std::string nm = f.path().stem().string();
            const auto sp = nm.find(' ');
            nm = trim(sp == std::string::npos ? std::string() : nm.substr(sp + 1));
            if (nm.empty()) nm = "Combi " + std::to_string(idx + 1);
            found.emplace(idx, nm);
        }
        if (ec2) lib.errors.push_back("Cannot read " + b.name + ": " + ec2.message());
        for (const auto& [idx, nm] : found) {
            PatchRef r;
            r.bank = number;
            r.index = idx;
            r.name = nm;
            r.combi = true;
            b.patches.push_back(int(lib.patches.size()));
            lib.patches.push_back(std::move(r));
        }
        if (!b.patches.empty()) lib.banks.push_back(std::move(b));
    }
    return lib;
}

const BankInfo* PatchLibrary::bank(int number) const {
    for (const auto& b : banks)
        if (b.number == number) return &b;
    return nullptr;
}

int PatchLibrary::find(int bankNumber, int index) const {
    if (const auto* b = bank(bankNumber))
        for (int i : b->patches)
            if (patches[size_t(i)].index == index) return i;
    return -1;
}

int PatchLibrary::findByName(int bankNumber, const std::string& name) const {
    if (const auto* b = bank(bankNumber))
        for (int i : b->patches)
            if (patches[size_t(i)].name == name) return i;
    return -1;
}

std::string displayName(const std::string& raw) {
    std::string s = raw;
    const auto nul = s.find(char(0));
    if (nul != std::string::npos) s.resize(nul);
    while (!s.empty() && std::string("[]{}_ ").find(s[0]) != std::string::npos) s.erase(0, 1);
    return trim(s);
}

bool matchesQuery(const std::string& text, const std::string& query) {
    const std::string t = lower(text), q = lower(query);
    size_t p = 0;
    while (p < q.size()) {
        while (p < q.size() && q[p] == ' ') ++p;
        size_t e = q.find(' ', p);
        if (e == std::string::npos) e = q.size();
        if (e > p && t.find(q.substr(p, e - p)) == std::string::npos) return false;
        p = e;
    }
    return true;
}

}  // namespace hs2port
