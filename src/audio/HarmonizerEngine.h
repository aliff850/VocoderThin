#pragma once

#include "RingBuffer.h"
#include "PitchDetector.h"
#include "PitchShifter.h"
#include "FormantShifter.h"
#include "VocoderBank.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vthin {

enum ParamId : int {
    kParamDryLevel = 0,
    kParamHarmonyLevel = 1,
    kParamVocalTone = 2,
    kParamVocoderBlend = 3,
    kParamChoirDetune = 4,
    kParamStereoWidth = 5,
    kParamPitchMode = 6,
    kParamAttackRelease = 7,
    kParamReverbMix = 8,
    kParamNoiseGate = 9,
    kNumParams = 10
};

enum class InputSourceMode : int {
    kMicrophone = 0,
    kHostAudio = 1,
    kTestVocalGenerator = 2
};

/**
 * Complete 8-Voice Polyphonic MIDI Vocal Harmonizer & Hybrid Vocoder DSP Engine.
 * - Pitch-Tracked Lock Mode (default): locks harmony voices to exact held MIDI notes.
 * - Relative Interval Mode: shifts voice by semitones relative to C4 (MIDI 60).
 * - Includes per-voice SOLA pitch shifting, formant/tone control, 16-band channel vocoder,
 *   unison detune, stereo chord spread, stereo vocal reverb, and 60 FPS oscilloscope feed.
 */
class HarmonizerEngine {
public:
    static constexpr std::size_t kMaxVoices = 8;

    HarmonizerEngine() noexcept;

    void prepare(float sampleRate) noexcept;
    void reset() noexcept;

    // Parameter management (normalized [0.0 .. 1.0])
    void setParameterNormalized(int index, float value) noexcept;
    float getParameterNormalized(int index) const noexcept;
    static const char* getParameterName(int index) noexcept;
    void getParameterDisplay(int index, char* outText, std::size_t maxLen) const noexcept;
    static const char* getParameterLabel(int index) noexcept;

    // MIDI control (thread-safe via lock-free MIDI queue or direct audio-thread call)
    void noteOn(int midiNote, float velocity) noexcept;
    void noteOff(int midiNote) noexcept;
    void allNotesOff() noexcept;
    void setPitchBendSemitones(float semitones) noexcept;

    // Audio processing
    void processBlock(const float* inL, const float* inR,
                      const float* micSamples,
                      float* outL, float* outR,
                      std::size_t numSamples) noexcept;

    // Input source selection
    void setInputSourceMode(InputSourceMode mode) noexcept {
        inputSourceMode_.store(static_cast<int>(mode), std::memory_order_relaxed);
    }
    InputSourceMode getInputSourceMode() const noexcept {
        return static_cast<InputSourceMode>(inputSourceMode_.load(std::memory_order_relaxed));
    }

    void setInputGainDb(float db) noexcept {
        inputGainDb_.store(db, std::memory_order_relaxed);
    }
    float getInputGainDb() const noexcept {
        return inputGainDb_.load(std::memory_order_relaxed);
    }

    // Telemetry for UI visualization
    const ScopeBuffer<2048>& getScopeBuffer() const noexcept { return scopeBuffer_; }
    float getDetectedPitchHz() const noexcept { return detectedPitchHz_.load(std::memory_order_relaxed); }
    float getInputLevelRms() const noexcept { return inputLevelRms_.load(std::memory_order_relaxed); }
    float getOutputLevelRms() const noexcept { return outputLevelRms_.load(std::memory_order_relaxed); }
    bool isNoteActive(int midiNote) const noexcept;
    int getActiveVoiceCount() const noexcept;

private:
    struct Voice {
        bool active{false};
        bool releasing{false};
        int midiNote{60};
        float velocity{0.8f};
        float envelope{0.0f};
        float carrierPhase{0.0f};
        float subPhase{0.0f};
        float lfoPhase{0.0f};
        std::uint64_t age{0};
        PitchShifter shifter{};
    };

    struct CombFilter {
        std::vector<float> buf;
        std::size_t pos{0};
        float filterStore{0.0f};
        float feedback{0.78f};
        float damp{0.25f};

        void init(std::size_t size) {
            buf.assign(size, 0.0f);
            pos = 0;
            filterStore = 0.0f;
        }
        void clear() noexcept {
            std::fill(buf.begin(), buf.end(), 0.0f);
            filterStore = 0.0f;
        }
        float process(float input) noexcept {
            if (buf.empty()) return 0.0f;
            const float output = buf[pos];
            filterStore = output * (1.0f - damp) + filterStore * damp;
            buf[pos] = input + filterStore * feedback;
            if (++pos >= buf.size()) pos = 0;
            return output;
        }
    };

    struct AllpassFilter {
        std::vector<float> buf;
        std::size_t pos{0};

        void init(std::size_t size) {
            buf.assign(size, 0.0f);
            pos = 0;
        }
        void clear() noexcept {
            std::fill(buf.begin(), buf.end(), 0.0f);
        }
        float process(float input) noexcept {
            if (buf.empty()) return input;
            const float bufOut = buf[pos];
            const float output = -input + bufOut;
            buf[pos] = input + bufOut * 0.5f;
            if (++pos >= buf.size()) pos = 0;
            return output;
        }
    };

    float generateTestVocalSample() noexcept;
    void processReverb(float inL, float inR, float& outL, float& outR) noexcept;

    float sampleRate_{44100.0f};
    std::array<std::atomic<float>, kNumParams> params_{};
    std::atomic<int> inputSourceMode_{static_cast<int>(InputSourceMode::kMicrophone)};
    std::atomic<float> inputGainDb_{0.0f};

    std::array<Voice, kMaxVoices> voices_{};
    std::array< std::atomic<bool>, 128 > activeNotesBitmap_{};
    std::uint64_t voiceCounter_{0};
    float pitchBendSemitones_{0.0f};

    PitchDetector pitchDetector_{};
    FormantShifter formantShifterL_{};
    FormantShifter formantShifterR_{};
    VocoderBank vocoderL_{};
    VocoderBank vocoderR_{};

    // Input conditioning (80 Hz HPF + Noise Gate)
    float hpfStateX_{0.0f};
    float hpfStateY_{0.0f};
    float gateEnv_{0.0f};
    float gateGain_{1.0f};

    // Built-in test vocal synthesizer state
    float testPhase_{0.0f};
    float testVibPhase_{0.0f};
    std::uint32_t rngState_{0x12345678u};

    // Stereo reverb filters
    std::array<CombFilter, 4> combsL_{};
    std::array<CombFilter, 4> combsR_{};
    std::array<AllpassFilter, 2> allpassL_{};
    std::array<AllpassFilter, 2> allpassR_{};

    // UI telemetry
    ScopeBuffer<2048> scopeBuffer_{};
    std::atomic<float> detectedPitchHz_{220.0f};
    std::atomic<float> inputLevelRms_{0.0f};
    std::atomic<float> outputLevelRms_{0.0f};
};

} // namespace vthin
