#pragma once

#include <array>
#include <cstddef>

namespace vthin {

/**
 * Real-time Phase-Locked Dual-Grain (SOLA) Pitch Shifter with 4-point Hermite
 * cubic interpolation. Aligns incoming grain delay to the active grain's phase
 * at each window zero-point to eliminate comb-filtering and phase cancellation.
 */
class PitchShifter {
public:
    static constexpr std::size_t kBufferSize = 4096;
    static constexpr std::size_t kBufferMask = kBufferSize - 1;

    PitchShifter() noexcept;

    void prepare(float sampleRate) noexcept;
    void reset() noexcept;

    /**
     * Set target pitch ratio (e.g., 1.0 = unison, 2.0 = +1 octave, 0.5 = -1 octave).
     */
    void setPitchRatio(float ratio) noexcept;
    float getPitchRatio() const noexcept { return pitchRatio_; }

    /**
     * Process a single mono sample at the current pitch ratio.
     */
    float processSample(float input) noexcept;

private:
    float readCubic(float delaySamples) const noexcept;
    float findPhaseAlignedDelay(float targetDelay, float activeDelay) const noexcept;

    float sampleRate_{44100.0f};
    float grainSize_{1024.0f};
    float pitchRatio_{1.0f};

    std::array<float, kBufferSize> buffer_{};
    std::size_t writePos_{0};

    float phaseA_{0.0f};
    float delayA_{256.0f};
    float delayB_{768.0f};
};

} // namespace vthin
