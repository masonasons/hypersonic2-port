#pragma once
// The VST 2.4 binary interface, as far as hs2capture needs it: struct layouts, opcode numbers and flags, written
// from the publicly documented ABI (no Steinberg SDK code). Plugins are called with the C calling convention.
#include <cstdint>

#if defined(_WIN32)
#define VSTCALL __cdecl
#else
#define VSTCALL
#endif

struct AEffect;
typedef intptr_t(VSTCALL* AudioMasterCallback)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr,
                                               float opt);
typedef intptr_t(VSTCALL* DispatcherProc)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr,
                                          float opt);
typedef void(VSTCALL* ProcessProc)(AEffect*, float** inputs, float** outputs, int32_t frames);
typedef void(VSTCALL* ProcessDoubleProc)(AEffect*, double** inputs, double** outputs, int32_t frames);
typedef void(VSTCALL* SetParameterProc)(AEffect*, int32_t index, float value);
typedef float(VSTCALL* GetParameterProc)(AEffect*, int32_t index);
typedef AEffect*(VSTCALL* PluginEntryProc)(AudioMasterCallback host);

struct AEffect {
    int32_t magic;  // 'VstP'
    DispatcherProc dispatcher;
    ProcessProc process;  // deprecated accumulating process
    SetParameterProc setParameter;
    GetParameterProc getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID, version;
    ProcessProc processReplacing;
    ProcessDoubleProc processDoubleReplacing;
    char future[56];
};

constexpr int32_t kVstMagic = ('V' << 24) | ('s' << 16) | ('t' << 8) | 'P';

// AEffect::flags
constexpr int32_t effFlagsHasEditor = 1 << 0, effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5,
                  effFlagsIsSynth = 1 << 8;

// Plugin opcodes (dispatcher)
enum : int32_t {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effGetProgramName = 5,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
    effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19,
    effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48, effCanDo = 51, effIdle = 53,
    effGetVstVersion = 58, effStartProcess = 71, effStopProcess = 72,
};

// Host opcodes (audioMaster callback)
enum : int32_t {
    audioMasterAutomate = 0, audioMasterVersion = 1, audioMasterCurrentId = 2, audioMasterIdle = 3,
    audioMasterWantMidi = 6, audioMasterGetTime = 7, audioMasterProcessEvents = 8, audioMasterIOChanged = 13,
    audioMasterNeedIdle = 14, audioMasterSizeWindow = 15, audioMasterGetSampleRate = 16,
    audioMasterGetBlockSize = 17, audioMasterGetCurrentProcessLevel = 23, audioMasterGetAutomationState = 24,
    audioMasterGetVendorString = 32, audioMasterGetProductString = 33, audioMasterGetVendorVersion = 34,
    audioMasterCanDo = 37, audioMasterGetLanguage = 38, audioMasterGetDirectory = 41,
    audioMasterUpdateDisplay = 42, audioMasterBeginEdit = 43, audioMasterEndEdit = 44,
};

constexpr int32_t kVstMidiType = 1;

struct VstMidiEvent {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
};

// VstEvents with room for kMaxEvents pointers (the plugin reads numEvents of them).
constexpr int kMaxEvents = 64;
struct VstEventsN {
    int32_t numEvents;
    intptr_t reserved;
    VstMidiEvent* events[kMaxEvents];
};

struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
// VstTimeInfo::flags
constexpr int32_t kVstTransportChanged = 1, kVstTransportPlaying = 1 << 1, kVstNanosValid = 1 << 8,
                  kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstBarsValid = 1 << 11,
                  kVstTimeSigValid = 1 << 13;

struct ERect {
    int16_t top, left, bottom, right;
};
