#include "PluginProcessor.h"
#include "../ui/PluginEditorWin32.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vthin {

namespace {
struct PresetData {
    const char* name;
    float values[kNumParams];
};

constexpr PresetData kPresets[PluginProcessor::kNumPresets] = {
    {
        "Natural Vocal Choir",
        // Dry, Harm, Tone, Vocoder, Detune, Stereo, Mode(Lock=0), Env, Reverb, Gate
        {0.70f, 0.85f, 0.50f, 0.15f, 0.24f, 0.75f, 0.0f, 0.10f, 0.22f, 0.30f}
    },
    {
        "Daft Robot Vocoder",
        {0.00f, 1.00f, 0.56f, 1.00f, 0.35f, 0.85f, 0.0f, 0.05f, 0.28f, 0.32f}
    },
    {
        "Deep Baritone Gospel",
        {0.55f, 0.90f, 0.32f, 0.10f, 0.30f, 0.80f, 0.0f, 0.14f, 0.35f, 0.30f}
    },
    {
        "Shimmering Airy Choir",
        {0.60f, 0.92f, 0.68f, 0.25f, 0.42f, 0.95f, 0.0f, 0.18f, 0.42f, 0.30f}
    }
};

void copyVstString(char* dst, const char* src, std::size_t maxLen) noexcept {
    if (!dst || !src || maxLen == 0) return;
    std::strncpy(dst, src, maxLen - 1);
    dst[maxLen - 1] = '\0';
}
} // namespace

PluginProcessor::PluginProcessor(audioMasterCallback hostCallback) noexcept
    : hostCallback_(hostCallback) {
    std::memset(&effect_, 0, sizeof(AEffect));
    effect_.magic = kVstEffectMagic;
    effect_.dispatcher = &PluginProcessor::dispatcherThunk;
    effect_.processDeprecated = &PluginProcessor::processReplacingThunk;
    effect_.setParameter = &PluginProcessor::setParameterThunk;
    effect_.getParameter = &PluginProcessor::getParameterThunk;
    effect_.numPrograms = kNumPresets;
    effect_.numParams = kNumParams;
    effect_.numInputs = 2;
    effect_.numOutputs = 2;
    effect_.flags = effFlagsHasEditor | effFlagsCanReplacing | effFlagsIsSynth;
    effect_.ioRatio = 1.0f;
    effect_.object = this;
    effect_.uniqueID = 0x5654686E; // 'VThn'
    effect_.version = 1000;
    effect_.processReplacing = &PluginProcessor::processReplacingThunk;

    editorRect_.top = 0;
    editorRect_.left = 0;
    editorRect_.bottom = static_cast<int16_t>(PluginEditorWin32::kHeight);
    editorRect_.right = static_cast<int16_t>(PluginEditorWin32::kWidth);

    setProgram(0);
    editor_ = std::make_unique<PluginEditorWin32>(*this);
}

PluginProcessor::~PluginProcessor() noexcept {
    if (editor_) {
        editor_->close();
    }
    micDevice_.stop();
}

void PluginProcessor::setParameterAutomated(int index, float normalizedValue) noexcept {
    engine_.setParameterNormalized(index, normalizedValue);
    if (hostCallback_) {
        hostCallback_(&effect_, audioMasterAutomate, index, 0, nullptr, normalizedValue);
    }
}

void PluginProcessor::setProgram(int programIndex) noexcept {
    currentProgram_ = std::clamp(programIndex, 0, kNumPresets - 1);
    copyVstString(programNameBuf_, kPresets[currentProgram_].name, sizeof(programNameBuf_));
    for (int i = 0; i < kNumParams; ++i) {
        engine_.setParameterNormalized(i, kPresets[currentProgram_].values[i]);
    }
}

const char* PluginProcessor::getProgramName(int programIndex) const noexcept {
    const int idx = std::clamp(programIndex, 0, kNumPresets - 1);
    return kPresets[idx].name;
}

void PluginProcessor::cycleNextPreset() noexcept {
    setProgram((currentProgram_ + 1) % kNumPresets);
}

void PluginProcessor::cycleInputSourceMode() noexcept {
    const int cur = static_cast<int>(engine_.getInputSourceMode());
    const int next = (cur + 1) % 3;
    const auto mode = static_cast<InputSourceMode>(next);
    engine_.setInputSourceMode(mode);
    if (mode == InputSourceMode::kMicrophone && !micDevice_.isRunning()) {
        micDevice_.start(micDevice_.getCurrentDeviceIndex(), sampleRate_);
    }
}

void PluginProcessor::selectNextMicDevice() noexcept {
    const auto& devs = micDevice_.enumerateDevices();
    if (devs.empty()) return;
    const std::size_t nextIdx = (micDevice_.getCurrentDeviceIndex() + 1) % devs.size();
    micDevice_.start(nextIdx, sampleRate_);
    engine_.setInputSourceMode(InputSourceMode::kMicrophone);
}

std::string PluginProcessor::getInputSourceBannerText() const {
    switch (engine_.getInputSourceMode()) {
        case InputSourceMode::kMicrophone:
            return micDevice_.isRunning() ? "SRC: LIVE MIC" : "SRC: MIC (NO DEV)";
        case InputSourceMode::kHostAudio:
            return "SRC: HOST TRACK";
        case InputSourceMode::kTestVocalGenerator:
            return "SRC: TEST VOICE";
        default:
            return "SRC: LIVE MIC";
    }
}

std::string PluginProcessor::getMicDeviceBannerText() const {
    return micDevice_.getCurrentDeviceName();
}

void PluginProcessor::processMidiMessage(unsigned char status, unsigned char data1, unsigned char data2) noexcept {
    const unsigned char msgType = status & 0xF0u;
    if (msgType == 0x90u) {
        // Note On (velocity 0 = Note Off)
        if (data2 > 0) {
            engine_.noteOn(static_cast<int>(data1), static_cast<float>(data2) / 127.0f);
        } else {
            engine_.noteOff(static_cast<int>(data1));
        }
    } else if (msgType == 0x80u) {
        // Note Off
        engine_.noteOff(static_cast<int>(data1));
    } else if (msgType == 0xB0u) {
        // Control Change: All Sound Off (120) or All Notes Off (123)
        if (data1 == 120 || data1 == 123) {
            engine_.allNotesOff();
        }
    } else if (msgType == 0xE0u) {
        // Pitch Bend (14-bit value centered at 8192 -> +/- 2 semitones)
        const int bend = (static_cast<int>(data2) << 7) | static_cast<int>(data1);
        const float norm = static_cast<float>(bend - 8192) / 8192.0f;
        engine_.setPitchBendSemitones(norm * 2.0f);
    }
}

void PluginProcessor::handleVstEvents(const VstEvents* events) noexcept {
    if (!events) return;
    for (int32_t i = 0; i < events->numEvents; ++i) {
        const VstEvent* ev = events->events[i];
        if (ev && ev->type == kVstMidiEventType) {
            const auto* midi = reinterpret_cast<const VstMidiEvent*>(ev);
            processMidiMessage(midi->midiData[0], midi->midiData[1], midi->midiData[2]);
        }
    }
}

void PluginProcessor::processReplacing(float** inputs, float** outputs, int32_t sampleFrames) noexcept {
    if (!outputs || sampleFrames <= 0) return;

    float* outL = outputs[0];
    float* outR = outputs[1] ? outputs[1] : outputs[0];
    const float* inL = (inputs && inputs[0]) ? inputs[0] : nullptr;
    const float* inR = (inputs && inputs[1]) ? inputs[1] : inL;

    int32_t remaining = sampleFrames;
    int32_t offset = 0;

    while (remaining > 0) {
        const int32_t chunk = std::min<int32_t>(remaining, static_cast<int32_t>(micScratchBuffer_.size()));
        micDevice_.readSamples(micScratchBuffer_.data(), static_cast<std::size_t>(chunk));

        engine_.processBlock(inL ? (inL + offset) : nullptr,
                             inR ? (inR + offset) : nullptr,
                             micScratchBuffer_.data(),
                             outL + offset,
                             outR + offset,
                             static_cast<std::size_t>(chunk));

        offset += chunk;
        remaining -= chunk;
    }
}

intptr_t VTHIN_VST_CALL PluginProcessor::dispatcherThunk(AEffect* effect, int32_t opcode,
                                                         int32_t index, intptr_t value,
                                                         void* ptr, float opt) {
    if (!effect || !effect->object) return 0;
    auto* self = static_cast<PluginProcessor*>(effect->object);
    if (opcode == effClose) {
        self->dispatcher(opcode, index, value, ptr, opt);
        delete self;
        return 1;
    }
    return self->dispatcher(opcode, index, value, ptr, opt);
}

void VTHIN_VST_CALL PluginProcessor::processReplacingThunk(AEffect* effect, float** inputs,
                                                           float** outputs, int32_t sampleFrames) {
    if (!effect || !effect->object) return;
    static_cast<PluginProcessor*>(effect->object)->processReplacing(inputs, outputs, sampleFrames);
}

void VTHIN_VST_CALL PluginProcessor::setParameterThunk(AEffect* effect, int32_t index, float parameter) {
    if (!effect || !effect->object) return;
    static_cast<PluginProcessor*>(effect->object)->engine_.setParameterNormalized(index, parameter);
}

float VTHIN_VST_CALL PluginProcessor::getParameterThunk(AEffect* effect, int32_t index) {
    if (!effect || !effect->object) return 0.0f;
    return static_cast<PluginProcessor*>(effect->object)->engine_.getParameterNormalized(index);
}

intptr_t PluginProcessor::dispatcher(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    switch (opcode) {
        case effOpen:
            // Attempt to open default Windows microphone automatically on startup
            micDevice_.start(0, sampleRate_);
            return 0;

        case effClose:
            if (editor_) {
                editor_->close();
            }
            micDevice_.stop();
            return 0;

        case effSetProgram:
            setProgram(static_cast<int>(value));
            return 0;

        case effGetProgram:
            return currentProgram_;

        case effSetProgramName:
            if (ptr) {
                copyVstString(programNameBuf_, static_cast<const char*>(ptr), sizeof(programNameBuf_));
            }
            return 0;

        case effGetProgramName:
            if (ptr) {
                copyVstString(static_cast<char*>(ptr), programNameBuf_, 24);
            }
            return 0;

        case effGetParamLabel:
            if (ptr) {
                copyVstString(static_cast<char*>(ptr), HarmonizerEngine::getParameterLabel(index), 8);
            }
            return 0;

        case effGetParamDisplay:
            if (ptr) {
                engine_.getParameterDisplay(index, static_cast<char*>(ptr), 16);
            }
            return 0;

        case effGetParamName:
            if (ptr) {
                copyVstString(static_cast<char*>(ptr), HarmonizerEngine::getParameterName(index), 16);
            }
            return 0;

        case effSetSampleRate:
            if (opt > 8000.0f) {
                sampleRate_ = opt;
                engine_.prepare(sampleRate_);
                if (micDevice_.isRunning()) {
                    micDevice_.start(micDevice_.getCurrentDeviceIndex(), sampleRate_);
                }
            }
            return 0;

        case effSetBlockSize:
            return 0;

        case effMainsChanged:
            if (value == 0) {
                engine_.reset();
            }
            return 0;

        case effEditGetRect:
            if (ptr) {
                *static_cast<ERect**>(ptr) = &editorRect_;
                return 1;
            }
            return 0;

        case effEditOpen:
            if (editor_) {
                return editor_->open(reinterpret_cast<HWND>(ptr)) ? 1 : 0;
            }
            return 0;

        case effEditClose:
            if (editor_) {
                editor_->close();
            }
            return 0;

        case effEditIdle:
            if (editor_) {
                editor_->idle();
            }
            return 0;

        case effProcessEvents:
            handleVstEvents(static_cast<const VstEvents*>(ptr));
            return 1;

        case effGetPlugCategory:
            return 2; // kPlugCategSynth (required by VeSTige instrument host)

        case effGetEffectName:
            if (ptr) {
                copyVstString(static_cast<char*>(ptr), "VocoderThin Harmonizer", 32);
                return 1;
            }
            return 0;

        case effGetVendorString:
            if (ptr) {
                copyVstString(static_cast<char*>(ptr), "VocoderThin Audio", 32);
                return 1;
            }
            return 0;

        case effGetProductString:
            if (ptr) {
                copyVstString(static_cast<char*>(ptr), "VocoderThin MIDI Vocal Harmonizer", 48);
                return 1;
            }
            return 0;

        case effGetVendorVersion:
            return 1000;

        case effCanDo:
            if (ptr) {
                const char* canDo = static_cast<const char*>(ptr);
                if (std::strcmp(canDo, "receiveVstEvents") == 0 ||
                    std::strcmp(canDo, "receiveVstMidiEvent") == 0 ||
                    std::strcmp(canDo, "receiveVstTimeInfo") == 0 ||
                    std::strcmp(canDo, "plugAsChannelInsert") == 0 ||
                    std::strcmp(canDo, "plugAsSend") == 0) {
                    return 1;
                }
            }
            return 0;

        case effGetVstVersion:
            return 2400;

        default:
            return 0;
    }
}

} // namespace vthin

// Standard VST 2.4 DLL Entry Points for Windows (LMMS VeSTige & other DAWs)
VTHIN_VST_EXPORT vthin::AEffect* VTHIN_VST_CALL VSTPluginMain(vthin::audioMasterCallback audioMaster) {
    auto* plugin = new vthin::PluginProcessor(audioMaster);
    return plugin->getAEffect();
}

VTHIN_VST_EXPORT vthin::AEffect* VTHIN_VST_CALL main_plugin(vthin::audioMasterCallback audioMaster) {
    return VSTPluginMain(audioMaster);
}

