#pragma once

#include "Vst2Abi.h"
#include "../audio/HarmonizerEngine.h"
#include "../audio/MicInputDevice.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace vthin {

class PluginEditorWin32;

/**
 * VST 2.4 PluginProcessor wrapping HarmonizerEngine, MicInputDevice, and PluginEditorWin32.
 * Configured with effFlagsIsSynth so LMMS VeSTige forwards live MIDI Controller
 * and Piano Roll events while streaming stereo audio to the LMMS Mixer.
 */
class PluginProcessor {
public:
    static constexpr int kNumPresets = 4;

    explicit PluginProcessor(audioMasterCallback hostCallback) noexcept;
    ~PluginProcessor() noexcept;

    AEffect* getAEffect() noexcept { return &effect_; }

    HarmonizerEngine& getEngine() noexcept { return engine_; }
    const HarmonizerEngine& getEngine() const noexcept { return engine_; }

    MicInputDevice& getMicDevice() noexcept { return micDevice_; }
    const MicInputDevice& getMicDevice() const noexcept { return micDevice_; }

    // Parameter changes initiated by the GUI
    void setParameterAutomated(int index, float normalizedValue) noexcept;

    // Preset management
    void setProgram(int programIndex) noexcept;
    int getProgram() const noexcept { return currentProgram_; }
    const char* getProgramName(int programIndex) const noexcept;
    void cycleNextPreset() noexcept;

    // Microphone & input source management for UI controls
    void cycleInputSourceMode() noexcept;
    void selectNextMicDevice() noexcept;
    std::string getInputSourceBannerText() const;
    std::string getMicDeviceBannerText() const;

    // Raw MIDI message processing (shared by VST2 effProcessEvents and Standalone MIDI In)
    void processMidiMessage(unsigned char status, unsigned char data1, unsigned char data2) noexcept;

    // Audio processing entry point
    void processReplacing(float** inputs, float** outputs, int32_t sampleFrames) noexcept;

private:
    static intptr_t VTHIN_VST_CALL dispatcherThunk(AEffect* effect, int32_t opcode,
                                                   int32_t index, intptr_t value,
                                                   void* ptr, float opt);
    static void VTHIN_VST_CALL processReplacingThunk(AEffect* effect, float** inputs,
                                                     float** outputs, int32_t sampleFrames);
    static void VTHIN_VST_CALL setParameterThunk(AEffect* effect, int32_t index, float parameter);
    static float VTHIN_VST_CALL getParameterThunk(AEffect* effect, int32_t index);

    intptr_t dispatcher(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
    void handleVstEvents(const VstEvents* events) noexcept;

    AEffect effect_{};
    audioMasterCallback hostCallback_{nullptr};
    ERect editorRect_{};

    HarmonizerEngine engine_{};
    MicInputDevice micDevice_{};
    std::unique_ptr<PluginEditorWin32> editor_{};

    float sampleRate_{44100.0f};
    int currentProgram_{0};
    char programNameBuf_[32]{"Natural Vocal Choir"};
    std::array<float, 4096> micScratchBuffer_{};
};

} // namespace vthin

