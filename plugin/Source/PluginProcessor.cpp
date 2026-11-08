#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "PluginEditor.h"
#include "hs2/bank.h"
#include "hs2/patch.h"
#include "hs2/synth.h"

namespace hs2port {

namespace {

std::filesystem::path toPath(const juce::File& f) {
#if JUCE_WINDOWS
    return std::filesystem::path(f.getFullPathName().toWideCharPointer());
#else
    return std::filesystem::path(f.getFullPathName().toStdString());
#endif
}


constexpr int kEventCapacity = 4096;
constexpr int kChunkFrames = 4096;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

float memoryValue(int mode) { return mode == 0 ? 0.f : mode == 1 ? 0.5f : 1.f; }

PatchInfo makeInfo(const hs2::Patch& patch, int bank, int index, const juce::String& bankName,
                   const std::vector<std::string>& warnings) {
    PatchInfo inf;
    inf.empty = false;
    inf.bank = bank;
    inf.index = index;
    inf.name = fromLatin1(displayName(patch.name));
    if (inf.name.isEmpty()) inf.name = "Untitled patch";
    inf.bankName = bankName;
    inf.description = fromLatin1(patch.description);
    inf.controllers = fromLatin1(patch.controllers);
    inf.author = fromLatin1(patch.author);
    for (int k = 0; k < kNumKnobs; ++k) inf.knobNames[size_t(k)] = fromLatin1(patch.quickControls[size_t(k)]);
    for (const auto& w : warnings) inf.warnings.add(juce::String(w));
    return inf;
}

// ---- parameter text (what hosts and screen readers read)
juce::String percent(float v) { return juce::String(juce::roundToInt(v * 100.f)) + " %"; }
juce::String gainText(double g) {  // g: linear gain
    return g <= 0.0 ? juce::String("off") : juce::String(20.0 * std::log10(g), 1) + " dB";
}
float numberIn(const juce::String& t) { return t.retainCharacters("0123456789.-").getFloatValue(); }

}  // namespace

juce::String fromLatin1(const std::string& s) {
    juce::String out;
    out.preallocateBytes(s.size() + 1);
    for (unsigned char c : s) {
        if (c == 0) break;
        out += juce::String::charToString(juce::juce_wchar(c));
    }
    return out.trim();
}

std::string toLatin1(const juce::String& s) {
    std::string out;
    for (auto p = s.getCharPointer(); !p.isEmpty();) {
        const auto c = p.getAndAdvance();
        out += char(c < 256 ? c : '?');
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ GlobalSettings

GlobalSettings::GlobalSettings() {
    juce::PropertiesFile::Options o;
    o.applicationName = "HS2Port";
    o.filenameSuffix = ".settings";
    o.folderName = "HS2Port";
    o.osxLibrarySubFolder = "Application Support";
    o.commonToAllUsers = false;
    props_ = std::make_unique<juce::PropertiesFile>(o);
}

juce::File GlobalSettings::defaultContentFolder() {
#if JUCE_WINDOWS
    const auto reg = juce::WindowsRegistry::getValue(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Steinberg\\Hypersonic 2\\Content Location", {},
        juce::WindowsRegistry::WoW64_32bit);
    if (reg.isNotEmpty() && juce::File::isAbsolutePath(reg) && juce::File(reg).isDirectory()) return juce::File(reg);
    return juce::File("C:\\Hypersonic 2 Content");
#else
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Hypersonic 2 Content");
#endif
}

juce::File GlobalSettings::contentFolder() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto s = props_->getValue("contentFolder");
    if (s.isNotEmpty() && juce::File::isAbsolutePath(s)) return juce::File(s);
    return defaultContentFolder();
}

void GlobalSettings::setContentFolder(const juce::File& f) {
    std::lock_guard<std::mutex> lk(mu_);
    props_->setValue("contentFolder", f.getFullPathName());
    props_->saveIfNeeded();
}

int GlobalSettings::memoryMode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return juce::jlimit(0, 2, props_->getIntValue("memoryMode", 1));  // MID, the engine default (0.5)
}

void GlobalSettings::setMemoryMode(int m) {
    std::lock_guard<std::mutex> lk(mu_);
    props_->setValue("memoryMode", juce::jlimit(0, 2, m));
    props_->saveIfNeeded();
}

// ------------------------------------------------------------------------------------------------ SharedContent

std::shared_ptr<hs2::Content> SharedContent::get(const juce::File& folder) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto key = folder.getFullPathName();
    if (auto c = map_[key].lock()) return c;
    auto c = std::make_shared<hs2::Content>(toPath(folder));
    map_[key] = c;
    return c;
}

// ------------------------------------------------------------------------------------------------ parameters

// A host parameter mirroring one engine VST parameter (range 0..1, the engine's own value, unconverted).
class HS2Processor::EngineParameter : public juce::AudioParameterFloat {
public:
    EngineParameter(HS2Processor& owner, int exposedIdx, const juce::String& id, const juce::String& name, float def,
                    std::function<juce::String(float)> text, std::function<float(const juce::String&)> parse)
        : juce::AudioParameterFloat(juce::ParameterID(id, 1), name, juce::NormalisableRange<float>(0.f, 1.f), def,
                                    juce::AudioParameterFloatAttributes()
                                        .withStringFromValueFunction([text](float v, int) { return text(v); })
                                        .withValueFromStringFunction([parse](const juce::String& t) {
                                            return juce::jlimit(0.f, 1.f, parse(t));
                                        })),
          owner_(owner), idx_(exposedIdx) {}

    // Hyperknobs carry the loaded patch's own knob name, e.g. "Part 1 Hyperknob 4: Thump".
    juce::String getName(int maximumStringLength) const override {
        if (idx_ > 0) {
            const int part = (idx_ - 1) / kPartParams, pp = (idx_ - 1) % kPartParams;
            if (pp >= ppKnob1)
                return ("Part " + juce::String(part + 1) + " " + owner_.knobName(part, pp - ppKnob1))
                    .substring(0, maximumStringLength);
        }
        return juce::AudioParameterFloat::getName(maximumStringLength);
    }

private:
    HS2Processor& owner_;
    int idx_;
};

juce::String HS2Processor::knobName(int part, int k) const {
    std::lock_guard<std::mutex> lk(infoMu_);
    const auto& n = infos_[size_t(part)].knobNames[size_t(k)];
    return "Hyperknob " + juce::String(k + 1) + (n.isNotEmpty() ? ": " + n : juce::String());
}

juce::String HS2Processor::partSummary(int part) const {
    std::lock_guard<std::mutex> lk(infoMu_);
    const auto& inf = infos_[size_t(part)];
    return "Part " + juce::String(part + 1) + ": " + (inf.empty ? juce::String("empty") : inf.name);
}

// ------------------------------------------------------------------------------------------------ loader thread

class HS2Processor::LoaderThread : public juce::Thread {
public:
    explicit LoaderThread(HS2Processor& o) : juce::Thread("HS2 Port patch loader"), o_(o) {}
    void run() override {
        while (!threadShouldExit()) {
            if (o_.rescanRequested_.exchange(false)) o_.rescanLibrary();
            o_.loadBusy_ = true;  // set before taking work, so an offline render never sees a gap
            {
                std::lock_guard<std::mutex> serial(o_.loaderMu_);
                // MIDI program changes (queued by the audio thread)
                std::vector<hs2::Engine::ProgramRequest> midi;
                {
                    int s1, n1, s2, n2;
                    o_.midiFifo_.prepareToRead(o_.midiFifo_.getNumReady(), s1, n1, s2, n2);
                    for (int i = 0; i < n1; ++i) midi.push_back(o_.midiReqs_[size_t(s1 + i)]);
                    for (int i = 0; i < n2; ++i) midi.push_back(o_.midiReqs_[size_t(s2 + i)]);
                    o_.midiFifo_.finishedRead(n1 + n2);
                }
                for (const auto& m : midi) {
                    Request r;
                    r.kind = m.bank >= 120 ? Request::combi : Request::patch;  // banks 120.. hold combis
                    r.part = m.channel;
                    r.bank = m.bank;
                    r.index = m.program;
                    o_.handleRequest(r);
                }
                if (o_.requestPending_.exchange(false)) {
                    std::vector<Request> reqs;
                    std::unique_ptr<Rebuild> rb;
                    {
                        std::lock_guard<std::mutex> lk(o_.requestMu_);
                        reqs.swap(o_.requests_);
                        rb = std::move(o_.rebuild_);
                    }
                    if (rb != nullptr) o_.rebuildNow(*rb);
                    for (const auto& r : reqs) o_.handleRequest(r);
                }
            }
            o_.loadBusy_ = false;
            wait(15);
        }
    }

private:
    HS2Processor& o_;
};

// ------------------------------------------------------------------------------------------------ processor

HS2Processor::HS2Processor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    // Host parameters: master volume, then per part Mute, Level, Pan, Sends 1-4, Hyperknobs 1-6. Defaults are the
    // engine's own (FUN_1008bad0); the loaded patches set the real values.
    const double masterScale = (8.0 - 2.2 * 0.33) / 4.0;  // master gain = scale * v^2 (hype 0.33, the default)
    auto pctParse = [](const juce::String& t) { return numberIn(t) / 100.f; };
    auto add = [this](int i, std::unique_ptr<EngineParameter> p) {
        exposed_[size_t(i)] = p.get();
        addParameter(p.release());
    };
    add(0, std::make_unique<EngineParameter>(
               *this, 0, "master", "Master Volume", 0.5f,
               [masterScale](float v) { return gainText(masterScale * double(v) * double(v)); }, pctParse));
    for (int part = 0; part < kNumParts; ++part) {
        const juce::String pid = "p" + juce::String(part + 1) + "_", pname = "Part " + juce::String(part + 1) + " ";
        add(exposedIndex(part, ppMute),
            std::make_unique<EngineParameter>(
                *this, exposedIndex(part, ppMute), pid + "mute", pname + "Mute", 0.f,
                [](float v) { return v > 0.5f ? juce::String("muted") : juce::String("playing"); },
                [](const juce::String& t) {
                    const auto s = t.trim().toLowerCase();
                    return (s.startsWith("m") || s == "on" || numberIn(s) > 0.5f) ? 1.f : 0.f;
                }));
        // Level: part gain Level^2 (times the patch's trim); shown relative to full level.
        add(exposedIndex(part, ppLevel),
            std::make_unique<EngineParameter>(
                *this, exposedIndex(part, ppLevel), pid + "level", pname + "Level", 0.f,
                [](float v) { return v <= 0.f ? juce::String("off") : percent(v) + ", " + gainText(double(v) * v); },
                pctParse));
        add(exposedIndex(part, ppPan),
            std::make_unique<EngineParameter>(
                *this, exposedIndex(part, ppPan), pid + "pan", pname + "Pan", 0.5f,
                [](float v) {
                    const int p = juce::roundToInt((v - 0.5f) * 200.f);
                    return p == 0 ? juce::String("centre")
                                  : (p < 0 ? "left " + juce::String(-p) : "right " + juce::String(p));
                },
                [](const juce::String& t) {
                    const auto s = t.trim().toLowerCase();
                    const float n = std::abs(numberIn(s)) / 200.f;
                    return s.startsWith("l") ? 0.5f - n : s.startsWith("r") ? 0.5f + n : s.startsWith("c") ? 0.5f
                                                                                                         : numberIn(s) / 100.f;
                }));
        for (int k = 0; k < kNumSends; ++k)
            add(exposedIndex(part, ppSend1 + k),
                std::make_unique<EngineParameter>(
                    *this, exposedIndex(part, ppSend1 + k), pid + "send" + juce::String(k + 1),
                    pname + "Effect Send " + juce::String(k + 1), 0.f,
                    [](float v) { return v <= 0.f ? juce::String("off") : percent(v) + ", " + gainText(double(v) * v); },
                    pctParse));
        for (int k = 0; k < kNumKnobs; ++k)
            add(exposedIndex(part, ppKnob1 + k),
                std::make_unique<EngineParameter>(*this, exposedIndex(part, ppKnob1 + k),
                                                  pid + "knob" + juce::String(k + 1),
                                                  pname + "Hyperknob " + juce::String(k + 1), 0.f, percent, pctParse));
    }
    for (int i = 0; i < kNumExposed; ++i) {
        engineVal_[size_t(i)] = kNaN;
        hostSeen_[size_t(i)] = exposed_[size_t(i)]->getValue();
        echo_[size_t(i)] = kNaN;
        dirty_[size_t(i)] = false;
    }
    events_.reserve(kEventCapacity);
    interleaved_.assign(size_t(kChunkFrames) * 2, 0.f);
    cmdQueue_.reserve(64);
    cmdWork_.reserve(64);

    // As the original: a new instance loads the default patch 084/0 ("Welcome ToThe Matrix", HS2 Preview Patches) into
    // part 1 (plugin ctor 1008bad0: +0xb6d0 = 0x15400, loaded by the idle function 1008d2b0 through the program change
    // path 1008c100 -> 10089ef0, not a restore); a state restore replaces it (setChunk clears +0xb6d0).
    Rebuild rb;
    rb.setup.parts[0].kind = Setup::Part::patch;
    rb.setup.parts[0].bank = 84;
    rb.setup.parts[0].index = 0;
    submitRebuild(rb);
    rescanRequested_ = true;
    loader_ = std::make_unique<LoaderThread>(*this);
    loader_->startThread();
    startTimerHz(20);
}

HS2Processor::~HS2Processor() {
    stopTimer();
    loader_->stopThread(10000);
}

bool HS2Processor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

// ---- requests

void HS2Processor::submit(const Request& r) {
    {
        std::lock_guard<std::mutex> lk(requestMu_);
        // Coalesce: a newer load of the same part replaces an older one; a combi replaces every part.
        if (r.kind == Request::combi)
            requests_.erase(std::remove_if(requests_.begin(), requests_.end(),
                                           [](const Request& q) { return q.kind != Request::memory; }),
                            requests_.end());
        else if (r.kind == Request::patch || r.kind == Request::clear)
            requests_.erase(std::remove_if(requests_.begin(), requests_.end(),
                                           [&](const Request& q) {
                                               return (q.kind == Request::patch || q.kind == Request::clear) &&
                                                      q.part == r.part;
                                           }),
                            requests_.end());
        requests_.push_back(r);
    }
    requestPending_ = true;
    if (loader_ != nullptr) loader_->notify();
}

void HS2Processor::submitRebuild(const Rebuild& r) {
    {
        std::lock_guard<std::mutex> lk(requestMu_);
        requests_.clear();  // the rebuild describes the whole setup
        rebuild_ = std::make_unique<Rebuild>(r);
    }
    requestPending_ = true;
    if (loader_ != nullptr) loader_->notify();
}

void HS2Processor::requestPatch(int part, int bank, int index, bool explicitRequest) {
    Request r;
    r.kind = Request::patch;
    r.part = juce::jlimit(0, kNumParts - 1, part);
    r.bank = bank;
    r.index = index;
    r.explicitRequest = explicitRequest;
    if (auto lib = library()) {
        const int i = lib->find(bank, index);
        if (i >= 0) r.name = fromLatin1(lib->patches[size_t(i)].name);
    }
    submit(r);
}

void HS2Processor::requestCombi(int bank, int index, bool explicitRequest) {
    Request r;
    r.kind = Request::combi;
    r.bank = bank;
    r.index = index;
    r.explicitRequest = explicitRequest;
    if (auto lib = library()) {
        const int i = lib->find(bank, index);
        if (i >= 0) r.name = fromLatin1(lib->patches[size_t(i)].name);
    }
    submit(r);
}

void HS2Processor::requestClearPart(int part) {
    Request r;
    r.kind = Request::clear;
    r.part = juce::jlimit(0, kNumParts - 1, part);
    r.explicitRequest = true;
    submit(r);
}

void HS2Processor::setMemoryMode(int m) {
    settings_->setMemoryMode(m);
    Request r;
    r.kind = Request::memory;
    r.index = juce::jlimit(0, 2, m);
    submit(r);
}

bool HS2Processor::isLoading() const { return loadBusy_.load() || requestPending_.load(); }

void HS2Processor::setStatus(const juce::String& s) {
    {
        std::lock_guard<std::mutex> lk(infoMu_);
        status_ = s;
    }
    ++changeCounter;
}

juce::String HS2Processor::statusText() const {
    std::lock_guard<std::mutex> lk(infoMu_);
    return status_;
}

PatchInfo HS2Processor::partInfo(int part) const {
    std::lock_guard<std::mutex> lk(infoMu_);
    return infos_[size_t(juce::jlimit(0, kNumParts - 1, part))];
}

juce::String HS2Processor::combiName() const {
    std::lock_guard<std::mutex> lk(infoMu_);
    return combiName_;
}

void HS2Processor::setContentFolder(const juce::File& f) {
    settings_->setContentFolder(f);
    libraryReady_ = false;
    rescanRequested_ = true;
    submitRebuild(currentRebuild());
}

void HS2Processor::rescanLibrary() {
    const auto folder = settings_->contentFolder();
    auto lib = std::make_shared<PatchLibrary>(PatchLibrary::scan(toPath(folder)));
    const auto n = lib->patches.size();
    {
        std::lock_guard<std::mutex> lk(infoMu_);
        library_ = std::move(lib);
    }
    libraryReady_ = true;
    if (n == 0)
        setStatus("No patches found in the content folder " + folder.getFullPathName() +
                  ". Use the Choose content folder button in Settings.");
    ++changeCounter;
}

// ---- loading (loader thread)

bool HS2Processor::prepareCommand(const Request& req, hs2::Content& content, Command& cmd, PatchInfo* info,
                                  std::array<PatchInfo, kNumParts>* combiInfos, juce::String& error) {
    const auto lib = library();
    const float mem = memoryValue(settings_->memoryMode());
    // The bank or combi may have changed since a state was saved: find the entry by name if the number moved.
    int index = req.index;
    juce::String libName;  // the name the browser shows (patch bank directory / combi file name)
    juce::String bankName = (req.kind == Request::combi ? "Combi bank " : "Bank ") + juce::String(req.bank).paddedLeft('0', 3);
    if (lib != nullptr) {
        if (req.name.isNotEmpty()) {
            const int i = lib->find(req.bank, index);
            if (i < 0 || fromLatin1(lib->patches[size_t(i)].name) != req.name) {
                const int j = lib->findByName(req.bank, toLatin1(req.name));
                if (j >= 0) index = lib->patches[size_t(j)].index;
            }
        }
        const int i = lib->find(req.bank, index);
        if (i >= 0) libName = fromLatin1(lib->patches[size_t(i)].name);
        if (const auto* b = lib->bank(req.bank)) bankName = fromLatin1(b->name);
    }
    // Loading through a throwaway player reads the sample banks and wavetables into the shared caches, so the
    // engine's own load on the audio thread finds them there; it also collects the unsupported-feature warnings.
    auto warmUp = [&](const hs2::Patch& p) {
        hs2::PatchPlayer tmp(content, float(sampleRate_.load()));
        tmp.setMemoryMode(mem);
        tmp.load(p);
        return tmp.warnings();
    };
    try {
        switch (req.kind) {
            case Request::patch: {
                const auto path = content.patchBankPath(req.bank);
                if (path.empty()) throw std::runtime_error("bank " + std::to_string(req.bank) + " is not in the content folder");
                auto mb = hs2::MilanBank::open(path);
                if (mb->kind != 1) throw std::runtime_error("bank " + std::to_string(req.bank) + " is not a patch bank");
                if (index < 0 || size_t(index) >= mb->entries.size() || mb->entries[size_t(index)].slot1.size == 0)
                    throw std::runtime_error("patch " + std::to_string(index + 1) + " does not exist in this bank");
                auto patch = std::make_shared<hs2::Patch>(hs2::Patch::parse(mb->read(mb->entries[size_t(index)].slot1)));
                const auto warnings = warmUp(*patch);
                if (info != nullptr) {
                    *info = makeInfo(*patch, req.bank, index, bankName, warnings);
                    if (libName.isNotEmpty()) info->name = libName;
                }
                cmd.kind = Command::loadPatch;
                cmd.part = req.part;
                cmd.bank = req.bank;
                cmd.patch = std::move(patch);
                return true;
            }
            case Request::combi: {
                const auto path = hs2::combiPath(content.root(), req.bank, index);
                if (path.empty())
                    throw std::runtime_error("combi " + std::to_string(index + 1) + " does not exist in combi bank " +
                                             std::to_string(req.bank));
                auto combi = std::make_shared<hs2::Combi>(hs2::Combi::load(path));
                for (int p = 0; p < kNumParts; ++p) {
                    PatchInfo inf;
                    if (const auto& patch = combi->parts[size_t(p)]) {
                        const auto warnings = warmUp(*patch);
                        inf = makeInfo(*patch, -1, -1, {}, warnings);
                    }
                    if (combiInfos != nullptr) (*combiInfos)[size_t(p)] = inf;
                }
                if (info != nullptr) {
                    *info = PatchInfo();
                    info->empty = false;
                    info->bank = req.bank;
                    info->index = index;
                    info->bankName = bankName;
                    info->name = libName.isNotEmpty() ? libName : fromLatin1(combi->name);
                    if (info->name.isEmpty()) info->name = "Combi " + juce::String(index + 1);
                }
                cmd.kind = Command::loadCombi;
                cmd.bank = req.bank;
                cmd.combi = std::move(combi);
                return true;
            }
            case Request::clear:
                cmd.kind = Command::clearPart;
                cmd.part = req.part;
                return true;
            case Request::memory:
                cmd.kind = Command::memoryMode;
                cmd.value = memoryValue(req.index);
                return true;
        }
    } catch (const std::exception& e) {
        error = (req.kind == Request::combi ? "Could not load combi " : "Could not load patch ") +
                juce::String(index + 1) + " of " + bankName + ": " + juce::String(e.what());
        return false;
    }
    return false;
}

void HS2Processor::handleRequest(const Request& req) {
    const auto folder = settings_->contentFolder();
    if (!folder.isDirectory()) {
        setStatus("Hypersonic 2 content folder not found: " + folder.getFullPathName() +
                  ". Use the Choose content folder button in Settings.");
        if (req.explicitRequest) explicitLoadFailed = true;
        return;
    }
    auto content = sharedContent_->get(folder);
    Command cmd;
    PatchInfo info;
    std::array<PatchInfo, kNumParts> combiInfos;
    juce::String error;
    if (!prepareCommand(req, *content, cmd, &info, &combiInfos, error)) {
        setStatus(error);
        if (req.explicitRequest) explicitLoadFailed = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(infoMu_);
        const juce::String partName = "Part " + juce::String(req.part + 1);
        switch (req.kind) {
            case Request::patch:
                setup_.parts[size_t(req.part)] = {Setup::Part::patch, req.bank, info.index, info.name};
                infos_[size_t(req.part)] = info;
                status_ = partName + ": loaded " + info.name + ".";
                if (!info.warnings.isEmpty())
                    status_ << " Parts of this patch are not supported yet: " << info.warnings.joinIntoString("; ");
                break;
            case Request::combi: {
                setup_ = Setup();
                setup_.combiBank = req.bank;
                setup_.combiIndex = info.index;
                setup_.combiName = info.name;
                infos_ = combiInfos;
                combiName_ = info.name;
                int used = 0;
                juce::StringArray warn;
                for (const auto& pi : combiInfos)
                    if (!pi.empty) {
                        ++used;
                        if (!pi.warnings.isEmpty()) warn.add(pi.name + ": " + pi.warnings.joinIntoString("; "));
                    }
                status_ = "Loaded combi " + info.name + ", " + juce::String(used) + (used == 1 ? " part." : " parts.");
                if (!warn.isEmpty()) status_ << " Not supported yet: " << warn.joinIntoString(". ");
                break;
            }
            case Request::clear:
                setup_.parts[size_t(req.part)] = {Setup::Part::cleared, -1, -1, {}};
                infos_[size_t(req.part)] = PatchInfo();
                status_ = partName + " cleared.";
                break;
            case Request::memory: break;
        }
    }
    {
        const juce::SpinLock::ScopedLockType cl(cmdLock_);
        cmdQueue_.push_back(std::move(cmd));
    }
    ++infoGeneration_;
    ++changeCounter;
}

void HS2Processor::rebuildNow(const Rebuild& rb) {
    const auto folder = settings_->contentFolder();
    if (!folder.isDirectory()) {
        setStatus("Hypersonic 2 content folder not found: " + folder.getFullPathName() +
                  ". Use the Choose content folder button in Settings.");
        if (rb.explicitRequest) explicitLoadFailed = true;
        return;
    }
    auto act = std::make_unique<Active>();
    act->content = sharedContent_->get(folder);
    act->sampleRate = sampleRate_.load();
    act->engine = std::make_unique<hs2::Engine>(*act->content, float(act->sampleRate));
    auto& eng = *act->engine;
    const int mm = settings_->memoryMode();
    if (mm != 1) eng.setMemoryMode(memoryValue(mm));  // MID is the engine's default
    eng.setRingOut(ringOut_.load());
    eng.setCutOnLoad(cutOnLoad_.load());

    Setup setup = rb.setup;
    std::array<PatchInfo, kNumParts> infos;
    juce::String combiName;
    juce::StringArray errors;
    if (setup.combiBank >= 0) {
        Request r;
        r.kind = Request::combi;
        r.bank = setup.combiBank;
        r.index = setup.combiIndex;
        r.name = setup.combiName;
        Command cmd;
        PatchInfo inf;
        juce::String err;
        if (prepareCommand(r, *act->content, cmd, &inf, &infos, err)) {
            applyCommand(eng, cmd);
            setup.combiIndex = inf.index;
            combiName = setup.combiName = inf.name;
        } else {
            errors.add(err);
            setup.combiBank = setup.combiIndex = -1;
        }
    }
    for (int p = 0; p < kNumParts; ++p) {
        auto& sp = setup.parts[size_t(p)];
        if (sp.kind == Setup::Part::fromCombi) continue;
        Request r;
        r.kind = sp.kind == Setup::Part::patch ? Request::patch : Request::clear;
        r.part = p;
        r.bank = sp.bank;
        r.index = sp.index;
        r.name = sp.name;
        Command cmd;
        PatchInfo inf;
        juce::String err;
        if (prepareCommand(r, *act->content, cmd, &inf, nullptr, err)) {
            applyCommand(eng, cmd);
            infos[size_t(p)] = inf;  // empty for a cleared part
            if (sp.kind == Setup::Part::patch) {
                sp.index = inf.index;
                sp.name = inf.name;
            }
        } else {
            errors.add("Part " + juce::String(p + 1) + ": " + err);
            sp = Setup::Part();  // the combi's patch (if any) stays in this part
        }
    }
    for (int i = 0; i < kNumExposed; ++i)
        if (rb.valid[size_t(i)] && eng.getParameter(engineIndex(i)) != rb.values[size_t(i)])
            eng.setParameter(engineIndex(i), rb.values[size_t(i)]);

    {
        const juce::SpinLock::ScopedLockType sl(swapLock_);
        std::swap(active_, act);
        for (int i = 0; i < kNumExposed; ++i) engineVal_[size_t(i)] = active_->engine->getParameter(engineIndex(i));
        resyncAll_ = true;
        const juce::SpinLock::ScopedLockType cl(cmdLock_);
        cmdQueue_.clear();  // the rebuild includes every load queued before it
    }
    act.reset();  // the previous engine is freed here, not on the audio thread

    {
        std::lock_guard<std::mutex> lk(infoMu_);
        setup_ = setup;
        infos_ = infos;
        combiName_ = combiName;
        juce::StringArray loaded;
        for (int p = 0; p < kNumParts; ++p)
            if (!infos[size_t(p)].empty) loaded.add("part " + juce::String(p + 1) + " " + infos[size_t(p)].name);
        status_ = errors.isEmpty() ? (combiName.isNotEmpty() ? "Loaded combi " + combiName + ". " : juce::String()) +
                                         (loaded.isEmpty() ? juce::String("All parts are empty.")
                                                           : "Loaded " + loaded.joinIntoString(", ") + ".")
                                   : errors.joinIntoString(" ");
    }
    if (!errors.isEmpty() && rb.explicitRequest) explicitLoadFailed = true;
    ++infoGeneration_;
    ++changeCounter;
}

void HS2Processor::applyCommand(hs2::Engine& e, const Command& c) {
    switch (c.kind) {
        case Command::loadPatch: e.loadPatch(c.part, *c.patch, c.bank); break;
        case Command::loadCombi: e.loadCombi(*c.combi); break;
        case Command::clearPart: e.clearPart(c.part); break;
        case Command::memoryMode: e.setMemoryMode(c.value); break;
    }
}

// ---- parameter sync

std::array<float, kNumExposed> HS2Processor::currentValues(std::array<bool, kNumExposed>* valid) const {
    std::array<float, kNumExposed> v{};
    for (int i = 0; i < kNumExposed; ++i) {
        const float h = exposed_[size_t(i)]->getValue();
        // A host change the audio thread has not applied yet counts; otherwise the engine's value.
        const bool hostPending = h != hostSeen_[size_t(i)].load() && h != echo_[size_t(i)].load();
        v[size_t(i)] = hostPending ? h : engineVal_[size_t(i)].load();
        if (valid != nullptr) (*valid)[size_t(i)] = !std::isnan(v[size_t(i)]);
    }
    return v;
}

HS2Processor::Rebuild HS2Processor::currentRebuild() const {
    Rebuild rb;
    {
        std::lock_guard<std::mutex> lk(infoMu_);
        rb.setup = setup_;
    }
    rb.values = currentValues(&rb.valid);
    return rb;
}

void HS2Processor::syncParameters(hs2::Engine& eng) {
    const bool all = resyncAll_.exchange(false);
    bool any = false;
    for (int i = 0; i < kNumExposed; ++i) {
        const int idx = engineIndex(i);
        const float e = eng.getParameter(idx);
        const float h = exposed_[size_t(i)]->getValue();
        if (all || e != engineVal_[size_t(i)].load(std::memory_order_relaxed)) {
            // The engine changed (a load, a combi, MIDI): the host parameter follows (message thread).
            engineVal_[size_t(i)] = e;
            hostSeen_[size_t(i)] = h;
            dirty_[size_t(i)] = true;
            any = true;
        } else if (h != hostSeen_[size_t(i)].load(std::memory_order_relaxed)) {
            hostSeen_[size_t(i)] = h;
            if (h == echo_[size_t(i)].load()) {
                echo_[size_t(i)] = kNaN;  // our own push from the engine's value: nothing to apply
            } else if (h != e) {
                eng.setParameter(idx, h);  // automation or the editor
                engineVal_[size_t(i)] = h;
            }
        }
    }
    if (any) anyDirty_ = true;
}

void HS2Processor::timerCallback() {
    if (anyDirty_.exchange(false))
        for (int i = 0; i < kNumExposed; ++i)
            if (dirty_[size_t(i)].exchange(false)) {
                const float v = engineVal_[size_t(i)].load();
                auto* p = exposed_[size_t(i)];
                if (!std::isnan(v) && p->getValue() != v) {
                    echo_[size_t(i)] = v;
                    p->setValueNotifyingHost(v);
                }
            }
    const int gen = infoGeneration_.load();
    if (gen != seenInfoGeneration_) {  // patch names changed: the Hyperknob parameter names follow
        seenInfoGeneration_ = gen;
        updateHostDisplay(ChangeDetails().withParameterInfoChanged(true));
    }
}

// ---- audio

void HS2Processor::prepareToPlay(double sampleRate, int) {
    sampleRate_ = sampleRate;
    std::lock_guard<std::mutex> serial(loaderMu_);  // the loader may be building with the old rate: wait for it
    bool rebuild = false;
    {
        const juce::SpinLock::ScopedLockType sl(swapLock_);
        if (active_ != nullptr) {
            if (active_->sampleRate != sampleRate) rebuild = true;
            else active_->engine->resume();
        }
    }
    if (rebuild) rebuildNow(currentRebuild());  // the engine is built for one sample rate
}

void HS2Processor::releaseResources() {}

void HS2Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    // No ScopedNoDenormals: the original never flushed denormals (the engine keeps its own 1e-6 bias), and
    // flushing changes the output.
    const int frames = buffer.getNumSamples();
    buffer.clear();
    if (frames <= 0) return;

    // An offline render waits for pending loads, so it never starts silent or with the wrong patch.
    if (isNonRealtime())
        for (int i = 0; i < 20000 && (requestPending_.load() || loadBusy_.load() || midiFifo_.getNumReady() > 0); ++i)
            juce::Thread::sleep(1);

    const juce::SpinLock::ScopedTryLockType lock(swapLock_);
    if (!lock.isLocked() || active_ == nullptr) return;
    auto& eng = *active_->engine;

    // Loads prepared by the loader thread.
    {
        const juce::SpinLock::ScopedTryLockType cl(cmdLock_);
        if (cl.isLocked()) std::swap(cmdQueue_, cmdWork_);
    }
    if (eng.ringOut() != ringOut_.load()) eng.setRingOut(ringOut_.load());
    if (eng.cutOnLoad() != cutOnLoad_.load()) eng.setCutOnLoad(cutOnLoad_.load());
    for (const auto& c : cmdWork_) applyCommand(eng, c);
    cmdWork_.clear();

    syncParameters(eng);

    // MIDI: every channel goes to the engine, which routes it to its part (part N = channel N) and handles bank
    // select and program changes itself (they come back as load requests below).
    events_.clear();
    for (const auto meta : midi) {
        const auto* d = meta.data;
        if (meta.numBytes < 1 || d[0] < 0x80 || d[0] >= 0xf0) continue;
        if (events_.size() >= size_t(kEventCapacity)) continue;
        hs2::MidiEvent e;
        e.offset = juce::jlimit(0, frames - 1, meta.samplePosition);
        for (int i = 0; i < std::min(meta.numBytes, 3); ++i) e.data[i] = d[i];
        if ((d[0] & 0xf0) == 0xb0 && meta.numBytes >= 3 && (d[1] == 0 || d[1] == 32)) {  // bank select
            const int ch = d[0] & 0x0f;
            if (d[1] == 32) sawBankLsb_[size_t(ch)] = true;
            // Many senders put the bank number on CC0 alone. Until a channel has seen a CC32, a CC0 value is the
            // bank: it goes to the engine as CC0 = 0 + CC32 = value (the original's own scheme, bank = CC0*128+CC32).
            if (d[1] == 0 && !sawBankLsb_[size_t(ch)] && events_.size() + 1 < size_t(kEventCapacity)) {
                hs2::MidiEvent msb = e;
                msb.data[2] = 0;
                events_.push_back(msb);
                e.data[1] = 32;
            }
            const int b = bankSelect_[size_t(ch)].load(std::memory_order_relaxed);
            const int v = e.data[2] & 0x7f;
            if (d[1] != e.data[1]) bankSelect_[size_t(ch)] = v;  // CC0 taken as the bank
            else bankSelect_[size_t(ch)] = d[1] == 0 ? v * 128 + (b & 0x7f) : (b & 0x3f80) + v;
            bankChannel_ = ch;
        }
        events_.push_back(e);
    }
    // A VST3 program change (setCurrentProgram): a MIDI program change on the channel of the latest bank select,
    // after this block's MIDI (the bank select the host sent with it may be in this block).
    if (const int pc = pendingProgram_.exchange(-1); pc >= 0 && events_.size() < size_t(kEventCapacity)) {
        hs2::MidiEvent e;
        e.offset = frames - 1;
        e.data[0] = uint8_t(0xc0 | bankChannel_.load());
        e.data[1] = uint8_t(pc & 0x7f);
        events_.push_back(e);
    }

    hs2::HostTransport tr;
    bool haveTransport = false;
    if (auto* ph = getPlayHead()) {
        if (const auto pos = ph->getPosition()) {
            haveTransport = true;
            if (const auto bpm = pos->getBpm()) tr.bpm = *bpm; else tr.tempoValid = false;
            if (const auto ppq = pos->getPpqPosition()) tr.ppq = *ppq; else tr.ppqValid = false;
            if (const auto bar = pos->getPpqPositionOfLastBarStart()) tr.barStart = *bar; else tr.barValid = false;
            if (const auto sig = pos->getTimeSignature()) {
                tr.sigNum = sig->numerator;
                tr.sigDen = sig->denominator;
            } else {
                tr.sigValid = false;
            }
            tr.playing = pos->getIsPlaying();
        }
    }

    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getWritePointer(1);
    size_t ev = 0;
    for (int start = 0; start < frames; start += kChunkFrames) {
        const int n = std::min(kChunkFrames, frames - start);
        const size_t evBegin = ev;
        while (ev < events_.size() && events_[ev].offset < start + n) {
            events_[ev].offset -= start;
            ++ev;
        }
        std::fill(interleaved_.begin(), interleaved_.begin() + 2 * n, 0.f);
        // The host transport describes the block start; later chunks keep counting on their own.
        eng.processBlock(interleaved_.data(), n, events_.data() + evBegin, int(ev - evBegin),
                         haveTransport && start == 0 ? &tr : nullptr);
        for (int i = 0; i < n; ++i) {
            outL[start + i] = interleaved_[size_t(2 * i)];
            outR[start + i] = interleaved_[size_t(2 * i + 1)];
        }
    }

    // Program changes become loads on the loader thread.
    const auto reqs = eng.takeProgramRequests();
    if (!reqs.empty() && midiProgramChanges_.load()) {
        int s1, n1, s2, n2;
        midiFifo_.prepareToWrite(int(reqs.size()), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) midiReqs_[size_t(s1 + i)] = reqs[size_t(i)];
        for (int i = 0; i < n2; ++i) midiReqs_[size_t(s2 + i)] = reqs[size_t(n1 + i)];
        midiFifo_.finishedWrite(n1 + n2);
        loader_->notify();
    }
}

// ------------------------------------------------------------------------------------------------ programs

void HS2Processor::setCurrentProgram(int program) {
    if (program < 0 || program >= kNoProgram) return;
    pendingProgram_ = program;
}

const juce::String HS2Processor::getProgramName(int program) {
    if (program == kNoProgram) return "None";
    // The name in the bank of the latest bank select, as the program change would load it.
    const int bank = bankSelect_[size_t(bankChannel_.load())].load();
    if (const auto lib = library()) {
        const int i = lib->find(bank, program);
        if (i >= 0) return fromLatin1(displayName(lib->patches[size_t(i)].name));
    }
    return "Program " + juce::String(program);
}

// ------------------------------------------------------------------------------------------------ state

void HS2Processor::getStateInformation(juce::MemoryBlock& destData) {
    const auto rb = currentRebuild();
    juce::XmlElement xml("HS2Port");
    xml.setAttribute("stateVersion", 2);
    xml.setAttribute("selectedPart", selectedPart.load());
    xml.setAttribute("midiProgramChanges", midiProgramChanges_.load());
    xml.setAttribute("ringOut", ringOut_.load());
    xml.setAttribute("cutOnLoad", cutOnLoad_.load());
    auto* setup = xml.createNewChildElement("SETUP");
    if (rb.setup.combiBank >= 0) {
        setup->setAttribute("combiBank", rb.setup.combiBank);
        setup->setAttribute("combiIndex", rb.setup.combiIndex);
        setup->setAttribute("combiName", rb.setup.combiName);
    }
    for (int p = 0; p < kNumParts; ++p) {
        const auto& sp = rb.setup.parts[size_t(p)];
        if (sp.kind == Setup::Part::fromCombi) continue;
        auto* e = setup->createNewChildElement("PART");
        e->setAttribute("part", p + 1);
        e->setAttribute("kind", sp.kind == Setup::Part::patch ? "patch" : "cleared");
        if (sp.kind == Setup::Part::patch) {
            e->setAttribute("bank", sp.bank);
            e->setAttribute("patch", sp.index);
            e->setAttribute("name", sp.name);
        }
    }
    // Parameter values as the engine holds them (the host parameters may not have caught up yet).
    auto* params = xml.createNewChildElement("PARAMS");
    for (int i = 0; i < kNumExposed; ++i)
        if (rb.valid[size_t(i)]) {
            auto* e = params->createNewChildElement("P");
            e->setAttribute("id", exposed_[size_t(i)]->getParameterID());
            e->setAttribute("value", double(rb.values[size_t(i)]));
        }
    copyXmlToBinary(xml, destData);
}

void HS2Processor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName("HS2Port")) return;
    Rebuild rb;
    rb.explicitRequest = false;
    std::map<juce::String, int> ids;
    for (int i = 0; i < kNumExposed; ++i) ids[exposed_[size_t(i)]->getParameterID()] = i;
    if (xml->getIntAttribute("stateVersion", 1) < 2) {
        // Version 1 (one part): bank / patch / patchName and the knob values of part 1.
        auto& sp = rb.setup.parts[0];
        sp.kind = Setup::Part::patch;
        sp.bank = xml->getIntAttribute("bank", 31);
        sp.index = xml->getIntAttribute("patch", 0);
        sp.name = xml->getStringAttribute("patchName");
        for (auto* p : xml->getChildWithTagNameIterator("PARAM")) {
            const auto id = p->getStringAttribute("id");
            if (id.startsWith("knob")) {
                const int k = id.getTrailingIntValue() - 1;
                if (k >= 0 && k < kNumKnobs) {
                    const int i = exposedIndex(0, ppKnob1 + k);
                    rb.values[size_t(i)] = float(p->getDoubleAttribute("value"));
                    rb.valid[size_t(i)] = true;
                }
            }
        }
    } else {
        selectedPart = juce::jlimit(0, kNumParts - 1, xml->getIntAttribute("selectedPart", 0));
        midiProgramChanges_ = xml->getBoolAttribute("midiProgramChanges", true);
        ringOut_ = xml->getBoolAttribute("ringOut", true);
        cutOnLoad_ = xml->getBoolAttribute("cutOnLoad", true);
        if (const auto* setup = xml->getChildByName("SETUP")) {
            rb.setup.combiBank = setup->getIntAttribute("combiBank", -1);
            rb.setup.combiIndex = setup->getIntAttribute("combiIndex", -1);
            rb.setup.combiName = setup->getStringAttribute("combiName");
            for (auto* e : setup->getChildWithTagNameIterator("PART")) {
                const int p = e->getIntAttribute("part", 0) - 1;
                if (p < 0 || p >= kNumParts) continue;
                auto& sp = rb.setup.parts[size_t(p)];
                if (e->getStringAttribute("kind") == "patch") {
                    sp.kind = Setup::Part::patch;
                    sp.bank = e->getIntAttribute("bank", -1);
                    sp.index = e->getIntAttribute("patch", -1);
                    sp.name = e->getStringAttribute("name");
                } else {
                    sp.kind = Setup::Part::cleared;
                }
            }
        }
        if (const auto* params = xml->getChildByName("PARAMS"))
            for (auto* e : params->getChildWithTagNameIterator("P")) {
                const auto it = ids.find(e->getStringAttribute("id"));
                if (it == ids.end()) continue;
                rb.values[size_t(it->second)] = float(e->getDoubleAttribute("value"));
                rb.valid[size_t(it->second)] = true;
            }
    }
    submitRebuild(rb);
    ++changeCounter;
}

juce::AudioProcessorEditor* HS2Processor::createEditor() { return new HS2Editor(*this); }

}  // namespace hs2port

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new hs2port::HS2Processor(); }
