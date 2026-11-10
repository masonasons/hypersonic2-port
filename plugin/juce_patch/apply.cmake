# Patches the build's own copy of JUCE 9.0.5 or 7.0.12 (FetchContent PATCH_COMMAND; the working directory is the JUCE
# source folder). Never touches a local JUCE checkout: FetchContent clones it first. The patched texts are the same
# in both versions; only the extensions header moved (JUCE 7: juce_audio_processors/utilities). JUCE 7 also gets
# the combo box accessible-name fix (below).
#
# What it adds: MIDI program changes per MIDI channel in the VST3 plugin. VST3 has no MIDI program change event; a
# host maps a program change on MIDI channel c to the program change parameter of the unit that
# IUnitInfo::getUnitByBus (kEvent, kInput, 0, c) returns. JUCE has a single program list on the root unit, so all
# channels share one parameter and program changes sent on several channels at once are lost. With the patch, a
# plugin whose VST3ClientExtensions::getPluginHasProgramChangePerMidiChannel() returns true gets one unit, one program
# list (128 programs) and one program change parameter per MIDI channel; the host's program change on channel c calls
# VST3ClientExtensions::setProgramForMidiChannel (c, program).
cmake_minimum_required(VERSION 3.22)

set(ext_file "modules/juce_audio_processors_headless/utilities/juce_VST3ClientExtensions.h")
if(NOT EXISTS "${ext_file}")
    set(ext_file "modules/juce_audio_processors/utilities/juce_VST3ClientExtensions.h")  # JUCE 7
endif()
set(vst3_file "modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp")

function(hs2_patch_file file)
    file(READ "${file}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    if(content MATCHES "HS2 patch")
        message(STATUS "HS2: ${file} already patched")
        return()
    endif()
    set(i 0)
    list(LENGTH ARGN n)
    while(i LESS n)
        list(GET ARGN ${i} old_var)
        math(EXPR j "${i} + 1")
        list(GET ARGN ${j} new_var)
        string(FIND "${content}" "${${old_var}}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR "HS2 JUCE patch: text not found in ${file} (${old_var}); is this JUCE 9.0.5 or 7.0.12?")
        endif()
        string(REPLACE "${${old_var}}" "${${new_var}}" content "${content}")
        math(EXPR i "${i} + 2")
    endwhile()
    file(WRITE "${file}" "${content}")
    message(STATUS "HS2: patched ${file}")
endfunction()

# ---- juce_VST3ClientExtensions.h: the plugin side of the per-channel program changes
set(e1_old [=[    virtual bool getPluginHasMainInput() const  { return true; }
]=])
set(e1_new [=[    virtual bool getPluginHasMainInput() const  { return true; }

    /** HS2 patch: return true to get one unit, program list and program change parameter per MIDI channel, so that a
        host's MIDI program change on a channel arrives as setProgramForMidiChannel (channel 0..15, program 0..127).
    */
    virtual bool getPluginHasProgramChangePerMidiChannel() const  { return false; }
    /** HS2 patch: a program change for MIDI channel `channel` (0..15). Called from the edit controller (any thread). */
    virtual void setProgramForMidiChannel (int /*channel*/, int /*program*/) {}
    /** HS2 patch: the name of program `program` in the program list of MIDI channel `channel`. */
    virtual String getProgramNameForMidiChannel (int /*channel*/, int program)  { return "Program " + String (program); }
]=])

# ---- juce_audio_plugin_client_VST3.cpp
# JuceAudioProcessor (IUnitInfo): constants and a helper
set(v1_old [=[        paramBypass               = 0x62797073  // 'byps'
    };
]=])
set(v1_new [=[        paramBypass               = 0x62797073  // 'byps'
    };

    // HS2 patch: one unit / program list / program change parameter per MIDI channel (ids + channel 1..16)
    static constexpr Vst::UnitID channelUnitBase = 0x68733200;            // 'hs2\0'
    static constexpr Vst::ProgramListID channelListBase = 0x68736c00;     // 'hsl\0'
    static constexpr Vst::ParamID channelProgramParamBase = 0x68737000;   // 'hsp\0'

    VST3ClientExtensions* channelProgramExtensions() const
    {
        if (audioProcessor != nullptr)
            if (auto* e = audioProcessor->getVST3ClientExtensions())
                if (e->getPluginHasProgramChangePerMidiChannel())
                    return e;

        return nullptr;
    }

    int channelForProgramList (Vst::ProgramListID listId) const
    {
        const auto c = (int) listId - (int) channelListBase - 1;
        return channelProgramExtensions() != nullptr && isPositiveAndBelow (c, 16) ? c : -1;
    }
]=])
set(v2_old [=[    Steinberg::int32 PLUGIN_API getUnitCount() override
    {
        return parameterGroups.size() + 1;
    }
]=])
set(v2_new [=[    Steinberg::int32 PLUGIN_API getUnitCount() override
    {
        return parameterGroups.size() + 1 + (channelProgramExtensions() != nullptr ? 16 : 0);
    }
]=])
set(v3_old [=[        if (auto* group = parameterGroups[unitIndex - 1])
        {
            info.id             = JuceAudioProcessor::getUnitID (group);]=])
set(v3_new [=[        if (const auto c = (int) unitIndex - 1 - parameterGroups.size(); channelProgramExtensions() != nullptr && isPositiveAndBelow (c, 16))
        {
            info.id             = channelUnitBase + (Vst::UnitID) (c + 1);
            info.parentUnitId   = Vst::kRootUnitId;
            info.programListId  = channelListBase + (Vst::ProgramListID) (c + 1);
            toString128 (info.name, "MIDI Channel " + String (c + 1));
            return kResultTrue;
        }

        if (auto* group = parameterGroups[unitIndex - 1])
        {
            info.id             = JuceAudioProcessor::getUnitID (group);]=])
set(v4_old [=[    Steinberg::int32 PLUGIN_API getProgramListCount() override
    {
        if (audioProcessor->getNumPrograms() > 0)
            return 1;

        return 0;
    }

    tresult PLUGIN_API getProgramListInfo (Steinberg::int32 listIndex, Vst::ProgramListInfo& info) override
    {
        if (listIndex == 0)
        {]=])
set(v4_new [=[    Steinberg::int32 PLUGIN_API getProgramListCount() override
    {
        return (audioProcessor->getNumPrograms() > 0 ? 1 : 0) + (channelProgramExtensions() != nullptr ? 16 : 0);
    }

    tresult PLUGIN_API getProgramListInfo (Steinberg::int32 listIndex, Vst::ProgramListInfo& info) override
    {
        if (const auto c = (int) listIndex - (audioProcessor->getNumPrograms() > 0 ? 1 : 0);
            channelProgramExtensions() != nullptr && isPositiveAndBelow (c, 16))
        {
            info.id = channelListBase + (Vst::ProgramListID) (c + 1);
            info.programCount = 128;
            toString128 (info.name, "MIDI Channel " + String (c + 1));
            return kResultTrue;
        }

        if (listIndex == 0 && audioProcessor->getNumPrograms() > 0)
        {]=])
set(v5_old [=[    tresult PLUGIN_API getProgramName (Vst::ProgramListID listId, Steinberg::int32 programIndex, Vst::String128 name) override
    {
        if (listId == static_cast<Vst::ProgramListID> (programParamID)]=])
set(v5_new [=[    tresult PLUGIN_API getProgramName (Vst::ProgramListID listId, Steinberg::int32 programIndex, Vst::String128 name) override
    {
        if (const auto c = channelForProgramList (listId); c >= 0 && isPositiveAndBelow ((int) programIndex, 128))
        {
            toString128 (name, channelProgramExtensions()->getProgramNameForMidiChannel (c, (int) programIndex));
            return kResultTrue;
        }

        if (listId == static_cast<Vst::ProgramListID> (programParamID)]=])
set(v6_old [=[    tresult PLUGIN_API getUnitByBus (Vst::MediaType, Vst::BusDirection, Steinberg::int32, Steinberg::int32, Vst::UnitID& unitId) override
    {
        unitId = Vst::kRootUnitId;
        return kResultOk;
    }]=])
set(v6_new [=[    tresult PLUGIN_API getUnitByBus (Vst::MediaType type, Vst::BusDirection dir, Steinberg::int32 busIndex,
                                     Steinberg::int32 channel, Vst::UnitID& unitId) override
    {
        unitId = Vst::kRootUnitId;

        if (channelProgramExtensions() != nullptr && type == Vst::kEvent && dir == Vst::kInput && busIndex == 0
            && isPositiveAndBelow ((int) channel, 16))
            unitId = channelUnitBase + (Vst::UnitID) (channel + 1);

        return kResultOk;
    }]=])
# the controller: the per-channel program change parameter class, and adding the 16 parameters
set(v7_old [=[        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramChangeParameter)
    };
]=])
set(v7_new [=[        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramChangeParameter)
    };

    // HS2 patch: the program change parameter of MIDI channel `channel`'s unit (see getUnitByBus). Every value the
    // host sets is passed on, also a repeated one (the same program number may come with another bank).
    struct ChannelProgramParameter final : public Vst::Parameter
    {
        ChannelProgramParameter (VST3ClientExtensions& e, int ch)
            : ext (e), channel (ch)
        {
            info.id = JuceAudioProcessor::channelProgramParamBase + (Vst::ParamID) (ch + 1);
            toString128 (info.title, "MIDI Channel " + String (ch + 1) + " Program");
            toString128 (info.shortTitle, "Ch" + String (ch + 1) + " Prg");
            toString128 (info.units, "");
            info.stepCount = 127;
            info.defaultNormalizedValue = 0.0;
            info.unitId = JuceAudioProcessor::channelUnitBase + (Vst::UnitID) (ch + 1);
            info.flags = Vst::ParameterInfo::kIsProgramChange | Vst::ParameterInfo::kIsList | Vst::ParameterInfo::kCanAutomate;
        }

        bool setNormalized (Vst::ParamValue v) override
        {
            ext.setProgramForMidiChannel (channel, jlimit (0, 127, roundToInt (v * 127.0)));
            valueNormalized = v;
            changed();
            return true;
        }

        void toString (Vst::ParamValue value, Vst::String128 result) const override
        {
            toString128 (result, ext.getProgramNameForMidiChannel (channel, jlimit (0, 127, roundToInt (value * 127.0))));
        }

        Vst::ParamValue toPlain (Vst::ParamValue v) const override       { return jlimit (0, 127, roundToInt (v * 127.0)); }
        Vst::ParamValue toNormalized (Vst::ParamValue v) const override  { return v / 127.0; }

    private:
        VST3ClientExtensions& ext;
        int channel;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelProgramParameter)
    };
]=])
set(v8_old [=[                    parameters.addParameter (new ProgramChangeParameter (*pluginInstance, audioProcessor->getProgramParamID()));
                }
]=])
set(v8_new [=[                    parameters.addParameter (new ProgramChangeParameter (*pluginInstance, audioProcessor->getProgramParamID()));
                }

                if (auto* ext = audioProcessor->channelProgramExtensions())
                    for (int ch = 0; ch < 16; ++ch)
                        parameters.addParameter (new ChannelProgramParameter (*ext, ch));
]=])

# ---- JUCE 7 only (the Windows 7 build): a combo box's accessible name is its title, as in JUCE 8 and 9. JUCE 7 uses
# the shown text, so a screen reader reads "1, combo box, 1" instead of "Output, combo box, 1". (Combo boxes without
# a title keep the text.)
set(combo_file "modules/juce_gui_basics/widgets/juce_ComboBox.cpp")
set(c1_old [=[    String getTitle() const override  { return comboBox.getText(); }]=])
set(c1_new [=[    // HS2 patch: the title (JUCE 8 and later), else the text
    String getTitle() const override  { return comboBox.getTitle().isNotEmpty() ? comboBox.getTitle() : comboBox.getText(); }]=])
file(READ "${combo_file}" combo_content)
string(REPLACE "\r\n" "\n" combo_content "${combo_content}")
string(FIND "${combo_content}" "${c1_old}" combo_pos)
if(NOT combo_pos EQUAL -1)
    hs2_patch_file("${combo_file}" c1_old c1_new)
endif()

hs2_patch_file("${ext_file}" e1_old e1_new)
hs2_patch_file("${vst3_file}" v1_old v1_new v2_old v2_new v3_old v3_new v4_old v4_new v5_old v5_new v6_old v6_new
               v7_old v7_new v8_old v8_new)
