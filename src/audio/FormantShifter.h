#pragma once

#include <array>

namespace vthin {

/**
 * Vocal Tone & Formant Shifter (-12 to +12 semitones).
 * Shapes the resonant vocal tract character (F1/F2/F3 vowel formants + spectral tilt)
 * so harmonized vocals can be darkened (deep chest/baritone tone) or brightened
 * (airy head/alto tone) independently of MIDI note pitch.
 */
class FormantShifter {
public:
    FormantShifter() noexcept;

    void prepare(float sampleRate) noexcept;
    void reset() noexcept;

    /**
     * Set formant/tone shift in semitones [-12.0f .. +12.0f].
     * 0.0f = neutral vocal timbre.
     */
    void setToneSemitones(float semitones) noexcept;
    float getToneSemitones() const noexcept { return toneSemitones_; }

    float processSample(float input) noexcept;

private:
    struct Biquad {
        float b0{1.0f}, b1{0.0f}, b2{0.0f};
        float a1{0.0f}, a2{0.0f};
        float z1{0.0f}, z2{0.0f};

        void reset() noexcept { z1 = 0.0f; z2 = 0.0f; }

        float process(float x) noexcept {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    void updateCoefficients() noexcept;
    static void setPeakingEq(Biquad& bq, float sampleRate, float freqHz, float q, float gainDb) noexcept;
    static void setHighShelf(Biquad& bq, float sampleRate, float freqHz, float gainDb) noexcept;
    static void setLowShelf(Biquad& bq, float sampleRate, float freqHz, float gainDb) noexcept;

    float sampleRate_{44100.0f};
    float toneSemitones_{0.0f};

    std::array<Biquad, 3> formantBands_{};
    Biquad lowShelf_{};
    Biquad highShelf_{};
};

} // namespace vthin

