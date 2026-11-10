#pragma once

#include <array>
#include <cstddef>

namespace vthin {

/**
 * 16-Band Channel Vocoder filter bank.
 * Extracts spectral envelopes from the microphone voice (modulator) across 16
 * logarithmically spaced bands (100 Hz - 7200 Hz) and applies them to a
 * polyphonic MIDI synthesizer carrier, with formant shift support and sibilance pass-through.
 */
class VocoderBank {
public:
    static constexpr std::size_t kNumBands = 16;

    VocoderBank() noexcept;

    void prepare(float sampleRate) noexcept;
    void reset() noexcept;

    /**
     * Shift carrier synthesis bands relative to modulator analysis bands in semitones [-12 .. +12].
     */
    void setFormantShiftSemitones(float semitones) noexcept;

    /**
     * Process one sample of modulator (voice) and carrier (polyphonic MIDI synth chord).
     */
    float processSample(float modulatorVoice, float carrierSynth) noexcept;

private:
    struct BiquadBandpass {
        float b0{0.0f}, b1{0.0f}, b2{0.0f};
        float a1{0.0f}, a2{0.0f};
        float z1a{0.0f}, z2a{0.0f};
        float z1b{0.0f}, z2b{0.0f};

        void reset() noexcept {
            z1a = z2a = z1b = z2b = 0.0f;
        }

        void configure(float sampleRate, float centerHz, float q) noexcept;

        float process(float x) noexcept {
            // 2-stage cascaded bandpass for steep 24 dB/oct selectivity
            const float y1 = b0 * x + z1a;
            z1a = b1 * x - a1 * y1 + z2a;
            z2a = b2 * x - a2 * y1;

            const float y2 = b0 * y1 + z1b;
            z1b = b1 * y1 - a1 * y2 + z2b;
            z2b = b2 * y1 - a2 * y2;
            return y2;
        }
    };

    void updateBands() noexcept;

    float sampleRate_{44100.0f};
    float formantSemitones_{0.0f};

    std::array<BiquadBandpass, kNumBands> modFilters_{};
    std::array<BiquadBandpass, kNumBands> carFilters_{};
    std::array<float, kNumBands> envelopes_{};

    float attackCoeff_{0.01f};
    float releaseCoeff_{0.001f};
    float sibilanceState_{0.0f};
};

} // namespace vthin

