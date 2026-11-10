#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vthin {

PitchDetector::PitchDetector() noexcept {
    reset();
}

void PitchDetector::prepare(float sampleRate) noexcept {
    sampleRate_ = (sampleRate > 8000.0f) ? sampleRate : 44100.0f;
    reset();
}

void PitchDetector::reset() noexcept {
    ring_.fill(0.0f);
    yinDiff_.fill(0.0f);
    yinCmndf_.fill(0.0f);
    writePos_ = 0;
    hopCounter_ = 0;
    detectedPitchHz_ = 220.0f;
    smoothedPitchHz_ = 220.0f;
    confidence_ = 0.0f;
    unvoicedHoldFrames_ = 0;
}

void PitchDetector::processSample(float sample) noexcept {
    ring_[writePos_ & (kBufferSize - 1)] = sample;
    ++writePos_;
    if (++hopCounter_ >= kHopSize) {
        hopCounter_ = 0;
        runYinAnalysis();
    }
}

void PitchDetector::processBlock(const float* samples, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        processSample(samples[i]);
    }
}

void PitchDetector::runYinAnalysis() noexcept {
    // Extract ordered window of kBufferSize samples ending at writePos_
    std::array<float, kBufferSize> frame{};
    float energy = 0.0f;
    for (std::size_t i = 0; i < kBufferSize; ++i) {
        const float s = ring_[(writePos_ + i) & (kBufferSize - 1)];
        frame[i] = s;
        energy += s * s;
    }

    const float rms = std::sqrt(energy / static_cast<float>(kBufferSize));
    if (rms < 0.002f) {
        if (unvoicedHoldFrames_ > 0) {
            --unvoicedHoldFrames_;
            confidence_ *= 0.85f;
        } else {
            confidence_ = 0.0f;
        }
        return;
    }

    // Limit lag search to vocal range: [75 Hz .. 900 Hz]
    const std::size_t minTau = std::max<std::size_t>(
        8, static_cast<std::size_t>(sampleRate_ / 900.0f));
    const std::size_t maxTau = std::min<std::size_t>(
        kHalfSize - 2, static_cast<std::size_t>(sampleRate_ / 75.0f));

    yinDiff_[0] = 0.0f;
    yinCmndf_[0] = 1.0f;

    float runningSum = 0.0f;
    for (std::size_t tau = 1; tau <= maxTau + 1; ++tau) {
        float diff = 0.0f;
        // Use 768-sample correlation window for fast, accurate real-time performance
        constexpr std::size_t kCorrLen = 768;
        for (std::size_t j = 0; j < kCorrLen; ++j) {
            const float d = frame[j] - frame[j + tau];
            diff += d * d;
        }
        yinDiff_[tau] = diff;
        runningSum += diff;
        yinCmndf_[tau] = (runningSum > 1e-12f)
            ? (diff * static_cast<float>(tau) / runningSum)
            : 1.0f;
    }

    // Find first local minimum below YIN threshold (0.18)
    constexpr float kThreshold = 0.18f;
    std::size_t bestTau = 0;
    float bestVal = 100.0f;

    for (std::size_t tau = minTau; tau <= maxTau; ++tau) {
        if (yinCmndf_[tau] < kThreshold) {
            while (tau + 1 <= maxTau && yinCmndf_[tau + 1] < yinCmndf_[tau]) {
                ++tau;
            }
            bestTau = tau;
            bestVal = yinCmndf_[tau];
            break;
        }
    }

    // Fallback to global minimum if below 0.38
    if (bestTau == 0) {
        for (std::size_t tau = minTau; tau <= maxTau; ++tau) {
            if (yinCmndf_[tau] < bestVal) {
                bestVal = yinCmndf_[tau];
                bestTau = tau;
            }
        }
        if (bestVal > 0.38f) {
            bestTau = 0;
        }
    }

    if (bestTau >= minTau && bestTau <= maxTau) {
        // Parabolic interpolation around bestTau
        const float y0 = yinCmndf_[bestTau - 1];
        const float y1 = yinCmndf_[bestTau];
        const float y2 = yinCmndf_[bestTau + 1];
        const float denom = 2.0f * (2.0f * y1 - y2 - y0);
        float delta = 0.0f;
        if (std::fabs(denom) > 1e-9f) {
            delta = (y2 - y0) / denom;
            delta = std::clamp(delta, -0.5f, 0.5f);
        }

        const float refinedTau = static_cast<float>(bestTau) + delta;
        const float freq = sampleRate_ / refinedTau;
        if (freq >= 70.0f && freq <= 950.0f) {
            detectedPitchHz_ = freq;
            confidence_ = std::clamp(1.0f - bestVal, 0.0f, 1.0f);
            unvoicedHoldFrames_ = 6;

            // Adaptive smoothing: jump quickly if interval is large, smooth micro-jitter
            const float ratio = freq / smoothedPitchHz_;
            if (ratio > 1.15f || ratio < 0.87f) {
                smoothedPitchHz_ = freq;
            } else {
                smoothedPitchHz_ = 0.65f * freq + 0.35f * smoothedPitchHz_;
            }
        }
    } else {
        if (unvoicedHoldFrames_ > 0) {
            --unvoicedHoldFrames_;
            confidence_ *= 0.85f;
        } else {
            confidence_ = 0.0f;
        }
    }
}

} // namespace vthin

