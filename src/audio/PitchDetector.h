#pragma once

#include <array>
#include <cstddef>

namespace vthin {

/**
 * Real-time monophonic vocal fundamental frequency (f0) detector using the
 * YIN (Cumulative Mean Normalized Difference Function) algorithm with
 * parabolic interpolation. Allocation-free during processSample().
 */
class PitchDetector {
public:
    static constexpr std::size_t kBufferSize = 2048;
    static constexpr std::size_t kHalfSize = 1024;
    static constexpr std::size_t kHopSize = 256;

    PitchDetector() noexcept;

    void prepare(float sampleRate) noexcept;
    void reset() noexcept;

    /**
     * Feed a single mono sample; updates pitch estimate every kHopSize samples.
     */
    void processSample(float sample) noexcept;

    /**
     * Feed a block of mono samples.
     */
    void processBlock(const float* samples, std::size_t count) noexcept;

    /**
     * Latest detected fundamental frequency in Hz (defaults to 220 Hz before lock).
     */
    float getDetectedPitchHz() const noexcept { return smoothedPitchHz_; }

    /**
     * Confidence in [0, 1] that the input signal is pitched/voiced.
     */
    float getConfidence() const noexcept { return confidence_; }

    /**
     * True if a voiced pitch was recently locked.
     */
    bool isVoiced() const noexcept { return confidence_ >= 0.55f; }

private:
    void runYinAnalysis() noexcept;

    float sampleRate_{44100.0f};
    std::array<float, kBufferSize> ring_{};
    std::array<float, kHalfSize> yinDiff_{};
    std::array<float, kHalfSize> yinCmndf_{};

    std::size_t writePos_{0};
    std::size_t hopCounter_{0};

    float detectedPitchHz_{220.0f};
    float smoothedPitchHz_{220.0f};
    float confidence_{0.0f};
    int unvoicedHoldFrames_{0};
};

} // namespace vthin

