#include "PitchShifter.h"

#include <algorithm>
#include <cmath>

namespace vthin {

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
}

PitchShifter::PitchShifter() noexcept {
    reset();
}

void PitchShifter::prepare(float sampleRate) noexcept {
    sampleRate_ = (sampleRate > 8000.0f) ? sampleRate : 44100.0f;
    grainSize_ = std::clamp(sampleRate_ * 0.025f, 512.0f, 1600.0f);
    reset();
}

void PitchShifter::reset() noexcept {
    buffer_.fill(0.0f);
    writePos_ = 0;
    phaseA_ = 0.0f;
    delayA_ = grainSize_ * 0.5f;
    delayB_ = grainSize_;
}

void PitchShifter::setPitchRatio(float ratio) noexcept {
    pitchRatio_ = std::clamp(ratio, 0.25f, 4.0f);
}

float PitchShifter::readCubic(float delaySamples) const noexcept {
    delaySamples = std::clamp(delaySamples, 4.0f, static_cast<float>(kBufferSize - 8));
    const float readIdx = static_cast<float>(writePos_) - delaySamples;
    const int idx0 = static_cast<int>(std::floor(readIdx));
    const float frac = readIdx - static_cast<float>(idx0);

    const float ym1 = buffer_[(idx0 - 1) & kBufferMask];
    const float y0  = buffer_[(idx0    ) & kBufferMask];
    const float y1  = buffer_[(idx0 + 1) & kBufferMask];
    const float y2  = buffer_[(idx0 + 2) & kBufferMask];

    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

float PitchShifter::findPhaseAlignedDelay(float targetDelay, float activeDelay) const noexcept {
    // Search within [-180 .. +180] samples around targetDelay for best waveform match with activeDelay
    const int baseTarget = static_cast<int>(std::round(targetDelay));
    const int baseActive = static_cast<int>(std::round(activeDelay));

    const float ref0 = buffer_[(static_cast<int>(writePos_) - baseActive) & kBufferMask];
    const float ref1 = buffer_[(static_cast<int>(writePos_) - baseActive - 4) & kBufferMask];
    const float ref2 = buffer_[(static_cast<int>(writePos_) - baseActive - 8) & kBufferMask];
    const float ref3 = buffer_[(static_cast<int>(writePos_) - baseActive - 12) & kBufferMask];
    const float refSlope = ref0 - ref1;

    float bestError = 1e30f;
    int bestOffset = 0;

    for (int offset = -160; offset <= 160; ++offset) {
        const int candDelay = baseTarget + offset;
        if (candDelay < 8 || candDelay > static_cast<int>(kBufferSize) - 32) {
            continue;
        }
        const int idx = static_cast<int>(writePos_) - candDelay;
        const float c0 = buffer_[idx & kBufferMask];
        const float c1 = buffer_[(idx - 4) & kBufferMask];
        const float cSlope = c0 - c1;
        // Match slope sign first to avoid 180-degree phase inversion
        if (cSlope * refSlope < 0.0f) {
            continue;
        }
        const float c2 = buffer_[(idx - 8) & kBufferMask];
        const float c3 = buffer_[(idx - 12) & kBufferMask];

        const float d0 = c0 - ref0;
        const float d1 = c1 - ref1;
        const float d2 = c2 - ref2;
        const float d3 = c3 - ref3;
        const float err = d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
        if (err < bestError) {
            bestError = err;
            bestOffset = offset;
        }
    }

    return std::clamp(static_cast<float>(baseTarget + bestOffset),
                      8.0f, static_cast<float>(kBufferSize - 16));
}

float PitchShifter::processSample(float input) noexcept {
    buffer_[writePos_ & kBufferMask] = input;

    float phaseB = phaseA_ + 0.5f;
    if (phaseB >= 1.0f) {
        phaseB -= 1.0f;
    }

    const float winA = 0.5f * (1.0f - std::cos(kTwoPi * phaseA_));
    const float winB = 0.5f * (1.0f - std::cos(kTwoPi * phaseB));

    const float sampleA = readCubic(delayA_);
    const float sampleB = readCubic(delayB_);
    const float out = winA * sampleA + winB * sampleB;

    // Advance both read delays at exact pitch rate
    const float delayStep = 1.0f - pitchRatio_;
    delayA_ += delayStep;
    delayB_ += delayStep;

    const float phaseStep = std::max(std::fabs(delayStep), 0.05f) / grainSize_;
    const float prevPhaseA = phaseA_;
    const float prevPhaseB = phaseB;

    phaseA_ += phaseStep;
    if (phaseA_ >= 1.0f) {
        phaseA_ -= 1.0f;
        // Grain A just hit window 0: reset delayA_ phase-aligned to active grain B
        const float targetA = (delayStep >= 0.0f) ? 32.0f : (grainSize_ + 32.0f);
        delayA_ = findPhaseAlignedDelay(targetA, delayB_);
    }

    const float nextPhaseB = prevPhaseB + phaseStep;
    if (nextPhaseB >= 1.0f && prevPhaseA < 0.5f) {
        // Grain B just hit window 0: reset delayB_ phase-aligned to active grain A
        const float targetB = (delayStep >= 0.0f) ? 32.0f : (grainSize_ + 32.0f);
        delayB_ = findPhaseAlignedDelay(targetB, delayA_);
    }

    // Safety bounds if pitchRatio is near 1.0
    delayA_ = std::clamp(delayA_, 6.0f, static_cast<float>(kBufferSize - 12));
    delayB_ = std::clamp(delayB_, 6.0f, static_cast<float>(kBufferSize - 12));

    ++writePos_;
    return out;
}

} // namespace vthin
