#pragma once

#include <cstdint>

#if defined(_WIN32)
    #define VTHIN_VST_CALL __cdecl
    #define VTHIN_VST_EXPORT extern "C" __declspec(dllexport)
#else
    #define VTHIN_VST_CALL
    #define VTHIN_VST_EXPORT extern "C"
#endif

namespace vthin {

constexpr int32_t kVstEffectMagic = 0x56737450; // 'VstP'
constexpr int32_t kVstMidiEventType = 1;

enum VstAEffectFlags : int32_t {
    effFlagsHasEditor     = 1 << 0,
    effFlagsCanReplacing  = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth       = 1 << 8
};

enum VstHostToPluginOpcodes : int32_t {
    effOpen              = 0,
    effClose             = 1,
    effSetProgram        = 2,
    effGetProgram        = 3,
    effSetProgramName    = 4,
    effGetProgramName    = 5,
    effGetParamLabel     = 6,
    effGetParamDisplay   = 7,
    effGetParamName      = 8,
    effSetSampleRate     = 10,
    effSetBlockSize      = 11,
    effMainsChanged      = 12,
    effEditGetRect       = 13,
    effEditOpen          = 14,
    effEditClose         = 15,
    effEditIdle          = 19,
    effProcessEvents     = 25,
    effGetPlugCategory   = 35,
    effGetEffectName     = 45,
    effGetVendorString   = 47,
    effGetProductString  = 48,
    effGetVendorVersion  = 49,
    effCanDo             = 51,
    effGetVstVersion     = 58
};

enum VstPluginToHostOpcodes : int32_t {
    audioMasterAutomate = 0,
    audioMasterVersion  = 1,
    audioMasterIdle     = 3
};

struct AEffect;

using audioMasterCallback = intptr_t(VTHIN_VST_CALL*)(AEffect* effect, int32_t opcode,
                                                      int32_t index, intptr_t value,
                                                      void* ptr, float opt);

using AEffectDispatcherProc = intptr_t(VTHIN_VST_CALL*)(AEffect* effect, int32_t opcode,
                                                        int32_t index, intptr_t value,
                                                        void* ptr, float opt);

using AEffectProcessProc = void(VTHIN_VST_CALL*)(AEffect* effect, float** inputs,
                                                 float** outputs, int32_t sampleFrames);

using AEffectProcessDoubleProc = void(VTHIN_VST_CALL*)(AEffect* effect, double** inputs,
                                                       double** outputs, int32_t sampleFrames);

using AEffectSetParameterProc = void(VTHIN_VST_CALL*)(AEffect* effect, int32_t index, float parameter);
using AEffectGetParameterProc = float(VTHIN_VST_CALL*)(AEffect* effect, int32_t index);

struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
};

struct VstEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
};

struct VstMidiEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    unsigned char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2];
};

struct AEffect {
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc processDeprecated;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
};

} // namespace vthin

