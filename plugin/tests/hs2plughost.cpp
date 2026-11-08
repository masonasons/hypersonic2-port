// hs2plughost: loads the built HS2 Port VST3 the way a DAW does and renders offline, to check the plugin path
// (loading, MIDI, transport, state) against hs2render, which is itself verified against the original.
//
//   hs2plughost <plugin.vst3> --bank B --patch P [--channel C] --notes "t:len:pitch:vel[:ch],..." [--lead 0.1]
//               [--tail 1] [--sr 44100] [--block 32] --out out.wav [--state-out state.bin] [--state-in state.bin]
//               [--knob k=value] [--param "name prefix=value"] [--no-pump] [--pc-param] [--state-attr name=value]
//
// The patch is selected with MIDI bank select (CC0 = 0, CC32 = bank, as the original reads it) + program change on
// channel C (default 1, so part C) in block 0; banks 120 and up select combis. The plugin loads it before block 1
// (offline renders wait for loads), so the output starts at block 1 and lines up with
//   hs2render --part "1:84:0" --load-bank --skip <block> --events "@<block>:load:C:B:P" --notes ... --lead L --tail T
//             --host "-1:120:0" --block <block>
// (a new instance holds the default patch 84/0 in part 1). With --state-in the setup comes from the saved state instead, and
// the output starts at block 0. Note channels default to C.
// --pc-param: the program change goes the way a VST3 host sends it: bank select as MIDI (CC0 / CC32 in block 0) and
// the program through the plugin's program list parameter (setCurrentProgram after block 0), which the plugin turns
// into a program change at the end of block 1; the patch then starts at block 2 (hs2render --skip 2*block,
// "@2*block:load:...").
// --pc-channels "ch:bank:prog,...": program changes on several MIDI channels at once, the way a VST3 host sends them
// with per-channel program lists: bank select as MIDI on each channel in block 0, then each channel's program through
// its own program change parameter ("MIDI Channel N Program", the patched JUCE) after block 0; the plugin adds them at
// the end of block 1 and the patches start at block 2 (as --pc-param).
// --init-cc "cc=value,...": controllers on channel C in block 0, right after the program change (as a MIDI file sends
// volume, pan and sends together with its program change); the plugin applies them to the new patch.
// --state-attr sets an attribute of the plugin's own state XML (e.g. ringOut=0) in the --state-in state before it is
// restored (inside the host's VST3PluginState wrapper: IComponent, base64, then the plugin's copyXmlToBinary block).
// The play head reports 120 bpm, stopped, position 0, 4/4 (as hs2render --host "-1:120:0").
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>
#include <fstream>
#include <vector>

namespace {

struct Note {
    double t, len;
    int pitch, vel, ch;
};

class FixedPlayHead : public juce::AudioPlayHead {
public:
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p;
        p.setBpm(120.0);
        p.setPpqPosition(0.0);
        p.setPpqPositionOfLastBarStart(0.0);
        p.setTimeSignature(TimeSignature{4, 4});
        p.setIsPlaying(false);
        return p;
    }
};

// The plugin's state inside a JUCE VST3 host state, with one attribute of its root element changed.
bool setStateAttribute(juce::MemoryBlock& state, const juce::String& name, const juce::String& value) {
    auto outer = juce::AudioProcessor::getXmlFromBinary(state.getData(), int(state.getSize()));  // the host's wrapper
    auto* comp = outer != nullptr ? outer->getChildByName("IComponent") : nullptr;
    if (comp == nullptr) return false;
    juce::MemoryBlock c;
    if (!c.fromBase64Encoding(comp->getAllSubText().trim())) return false;
    const auto* d = static_cast<const juce::uint8*>(c.getData());
    for (size_t pos = 0; pos + 8 <= c.getSize(); ++pos) {
        if (juce::ByteOrder::littleEndianInt(d + pos) != 0x21324356) continue;  // copyXmlToBinary's magic
        const size_t len = juce::ByteOrder::littleEndianInt(d + pos + 4);
        auto inner = juce::AudioProcessor::getXmlFromBinary(d + pos, int(c.getSize() - pos));
        if (inner == nullptr || pos + 8 + len > c.getSize()) return false;
        inner->setAttribute(name, value);
        juce::MemoryBlock nb;
        juce::AudioProcessor::copyXmlToBinary(*inner, nb);
        juce::MemoryBlock c2(d, pos);
        c2.append(nb.getData(), nb.getSize());
        c2.append(d + pos + 8 + len, c.getSize() - pos - 8 - len);
        comp->deleteAllTextElements();
        comp->addTextElement(c2.toBase64Encoding());
        state.reset();
        juce::AudioProcessor::copyXmlToBinary(*outer, state);
        return true;
    }
    return false;
}

void writeWav(const juce::File& f, const std::vector<float>& lr, int sr) {
    juce::FileOutputStream os(f);
    if (!os.openedOk()) return;
    os.setPosition(0);
    os.truncate();
    const auto dataBytes = juce::uint32(lr.size() * sizeof(float));
    os.write("RIFF", 4);
    os.writeInt(int(36 + dataBytes));
    os.write("WAVEfmt ", 8);
    os.writeInt(16);
    os.writeShort(3);  // IEEE float
    os.writeShort(2);
    os.writeInt(sr);
    os.writeInt(sr * 8);
    os.writeShort(8);
    os.writeShort(32);
    os.write("data", 4);
    os.writeInt(int(dataBytes));
    os.write(lr.data(), dataBytes);
}

}  // namespace

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(juce::String::fromUTF8(argv[i]));
    auto opt = [&](const juce::String& name, const juce::String& def) {
        const int i = args.indexOf(name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : def;
    };
    if (args.isEmpty()) {
        std::puts("usage: hs2plughost <plugin.vst3> --bank B --patch P --notes ... --out out.wav");
        return 2;
    }
    const juce::String pluginPath = args[0];
    const int bank = opt("--bank", "31").getIntValue(), patch = opt("--patch", "0").getIntValue();
    const int channel = juce::jlimit(1, 16, opt("--channel", "1").getIntValue());
    const double sr = opt("--sr", "44100").getDoubleValue(), lead = opt("--lead", "0.1").getDoubleValue(),
                 tail = opt("--tail", "1").getDoubleValue();
    const int block = opt("--block", "32").getIntValue();
    const juce::String stateIn = opt("--state-in", {}), stateOut = opt("--state-out", {});
    const bool pump = !args.contains("--no-pump");
    const bool pcParam = args.contains("--pc-param");
    struct ChannelPc { int ch, bank, prog; };
    std::vector<ChannelPc> pcChannels;
    for (const auto& tok : juce::StringArray::fromTokens(opt("--pc-channels", {}), ",", {})) {
        const auto f = juce::StringArray::fromTokens(tok, ":", {});
        if (f.size() == 3) pcChannels.push_back({f[0].getIntValue(), f[1].getIntValue(), f[2].getIntValue()});
    }  // --no-pump: never run the message loop (state saved "too early")

    std::vector<Note> notes;
    for (const auto& tok : juce::StringArray::fromTokens(opt("--notes", "0:1:60:100"), ",", {})) {
        const auto f = juce::StringArray::fromTokens(tok, ":", {});
        if (f.size() == 4 || f.size() == 5)
            notes.push_back({f[0].getDoubleValue(), f[1].getDoubleValue(), f[2].getIntValue(), f[3].getIntValue(),
                             f.size() == 5 ? f[4].getIntValue() : channel});
    }

    juce::AudioPluginFormatManager fm;
    fm.addFormat(std::make_unique<juce::VST3PluginFormat>());
    juce::OwnedArray<juce::PluginDescription> types;
    juce::KnownPluginList list;
    for (auto* format : fm.getFormats()) list.scanAndAddFile(pluginPath, true, types, *format);
    if (types.isEmpty()) {
        std::fprintf(stderr, "no plugin found in %s\n", pluginPath.toRawUTF8());
        return 1;
    }
    juce::String error;
    auto plugin = fm.createPluginInstance(*types[0], sr, block, error);
    if (plugin == nullptr) {
        std::fprintf(stderr, "could not load plugin: %s\n", error.toRawUTF8());
        return 1;
    }
    FixedPlayHead playHead;
    plugin->setPlayHead(&playHead);
    plugin->setNonRealtime(true);
    plugin->setPlayConfigDetails(0, 2, sr, block);
    if (stateIn.isNotEmpty()) {
        juce::MemoryBlock mb;
        juce::File(stateIn).loadFileAsData(mb);
        const auto attr = opt("--state-attr", {});
        if (attr.contains("=") && !setStateAttribute(mb, attr.upToFirstOccurrenceOf("=", false, false),
                                                     attr.fromFirstOccurrenceOf("=", false, false))) {
            std::fprintf(stderr, "could not change the state%s", juce::newLine.getDefault());
            return 1;
        }
        plugin->setStateInformation(mb.getData(), int(mb.getSize()));
    }
    plugin->prepareToPlay(sr, block);

    const int start = stateIn.isNotEmpty() ? 0 : (pcParam || !pcChannels.empty()) ? 2 * block : block;  // patch start
    double end = 0;
    for (const auto& n : notes) end = std::max(end, n.t + n.len);
    const int total = start + int((lead + end + tail) * sr);
    struct Ev {
        int frame;
        juce::MidiMessage msg;
    };
    std::vector<Ev> evs;
    if (stateIn.isEmpty() && !pcChannels.empty()) {
        for (const auto& c : pcChannels) {
            evs.push_back({0, juce::MidiMessage::controllerEvent(c.ch, 0, 0)});
            evs.push_back({0, juce::MidiMessage::controllerEvent(c.ch, 32, c.bank)});
        }
    } else if (stateIn.isEmpty()) {
        evs.push_back({0, juce::MidiMessage::controllerEvent(channel, 0, 0)});
        evs.push_back({0, juce::MidiMessage::controllerEvent(channel, 32, bank)});
        if (!pcParam) evs.push_back({0, juce::MidiMessage::programChange(channel, patch)});
        for (const auto& tok : juce::StringArray::fromTokens(opt("--init-cc", {}), ",", {}))
            if (tok.contains("="))
                evs.push_back({0, juce::MidiMessage::controllerEvent(channel, tok.upToFirstOccurrenceOf("=", false, false).getIntValue(),
                                                                     tok.fromFirstOccurrenceOf("=", false, false).getIntValue())});
    }
    for (const auto& n : notes) {
        evs.push_back({start + int((lead + n.t) * sr), juce::MidiMessage::noteOn(n.ch, n.pitch, juce::uint8(n.vel))});
        evs.push_back({start + int((lead + n.t + n.len) * sr), juce::MidiMessage::noteOff(n.ch, n.pitch)});
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });

    // Optional parameter change (--param "name prefix=value"; --knob k=value = "Part 1 Hyperknob k"), made after the
    // patch has loaded and the host parameters have taken its values (a load sets the patch's own settings).
    auto paramArg = opt("--param", {});
    if (paramArg.isEmpty() && opt("--knob", {}).contains("="))
        paramArg = "Part 1 Hyperknob " + opt("--knob", {});
    const int knobFrame = start + int(lead * sr / 2);
    bool knobDone = !paramArg.contains("=");

    std::vector<float> out;
    out.reserve(size_t(total) * 2);
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    size_t ei = 0;
    for (int t = 0; t < total; t += block) {
        const int n = std::min(block, total - t);
        buf.setSize(2, n, false, false, true);
        buf.clear();
        midi.clear();
        while (ei < evs.size() && evs[ei].frame < t + n) {
            midi.addEvent(evs[ei].msg, evs[ei].frame - t);
            ++ei;
        }
        if (!knobDone && t >= knobFrame) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            const auto prefix = paramArg.upToLastOccurrenceOf("=", false, false);
            const float v = paramArg.fromLastOccurrenceOf("=", false, false).getFloatValue();
            bool found = false;
            for (auto* p : plugin->getParameters()) {
                const auto name = p->getName(200);
                // "Part 1 Hyperknob 1" must not match "Part 1 Hyperknob 1x" or "Part 11 ...": the prefix ends the
                // name or is followed by ':'
                if (name == prefix || name.startsWith(prefix + ":")) {
                    p->setValueNotifyingHost(v);
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::fprintf(stderr, "no parameter named %s%s", prefix.toRawUTF8(), juce::newLine.getDefault());
                return 1;
            }
            knobDone = true;
        }
        plugin->processBlock(buf, midi);
        if (!pcChannels.empty() && stateIn.isEmpty() && t == 0) {
            for (const auto& c : pcChannels) {
                const auto want = "MIDI Channel " + juce::String(c.ch) + " Program";
                juce::AudioProcessorParameter* found = nullptr;
                for (auto* p : plugin->getParameters())
                    if (p->getName(100) == want) found = p;
                if (found == nullptr) {
                    std::fprintf(stderr, "no parameter %s%s", want.toRawUTF8(), juce::newLine.getDefault());
                    return 1;
                }
                found->setValueNotifyingHost(float(c.prog) / 127.f);
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        }
        if (pcParam && stateIn.isEmpty() && t == 0) {
            if (plugin->getNumPrograms() < 128) {
                std::fprintf(stderr, "the plugin has %d programs%s", plugin->getNumPrograms(), juce::newLine.getDefault());
                return 1;
            }
            plugin->setCurrentProgram(patch);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        }
        if (pump && (t / block) % 64 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(1);  // as a DAW's UI loop
        for (int i = 0; i < n; ++i) {
            out.push_back(buf.getSample(0, i));
            out.push_back(buf.getSample(1, i));
        }
    }
    // Drop the frames before the patch started, so the file lines up with hs2render.
    std::vector<float> aligned(out.begin() + std::min<size_t>(out.size(), size_t(start) * 2), out.end());
    writeWav(juce::File(opt("--out", "plughost.wav")), aligned, int(sr));

    if (pump) juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    if (stateOut.isNotEmpty()) {
        juce::MemoryBlock mb;
        plugin->getStateInformation(mb);
        juce::File(stateOut).replaceWithData(mb.getData(), mb.getSize());
    }
    plugin->releaseResources();
    plugin = nullptr;
    std::printf("rendered %d frames\n", int(aligned.size() / 2));
    return 0;
}
