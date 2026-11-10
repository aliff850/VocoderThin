#include "FormantShifter.h"

#include <algorithm>
#include <cmath>

namespace vthin {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

FormantShifter::FormantShifter() noexcept {
    prepare(44100.0f);
}

void FormantShifter::prepare(float sampleRate) noexcept {
    sampleRate_ = (sampleRate > 8000.0f) ? sampleRate : 44100.0f;
    reset();
    updateCoefficients();
}

void FormantShifter::reset() noexcept {
    for (auto& b : formantBands_) {
        b.reset();
    }
    lowShelf_.reset();
    highShelf_.reset();
}

void FormantShifter::setToneSemitones(float semitones) noexcept {
    const float clamped = std::clamp(semitones, -12.0f, 12.0f);
    if (std::fabs(clamped - toneSemitones_) > 0.02f) {
        toneSemitones_ = clamped;
        updateCoefficients();
    }
}

void FormantShifter::setPeakingEq(Biquad& bq, float sampleRate, float freqHz, float q, float gainDb) noexcept {
    freqHz = std::clamp(freqHz, 60.0f, sampleRate * 0.44f);
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPi * freqHz / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.0f * q);

    const float b0 = 1.0f + alpha * A;
    const float b1 = -2.0f * cosw;
    const float b2 = 1.0f - alpha * A;
    const float a0 = 1.0f + alpha / A;
    const float a1 = -2.0f * cosw;
    const float a2 = 1.0f - alpha / A;

    bq.b0 = b0 / a0;
    bq.b1 = b1 / a0;
    bq.b2 = b2 / a0;
    bq.a1 = a1 / a0;
    bq.a2 = a2 / a0;
}

void FormantShifter::setLowShelf(Biquad& bq, float sampleRate, float freqHz, float gainDb) noexcept {
    freqHz = std::clamp(freqHz, 60.0f, sampleRate * 0.44f);
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPi * freqHz / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float sqrtA = std::sqrt(A);
    const float alpha = sinw * 0.5f * std::sqrt(2.0f);

    const float a0 = (A + 1.0f) + (A - 1.0f) * cosw + 2.0f * sqrtA * alpha;
    bq.b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosw + 2.0f * sqrtA * alpha)) / a0;
    bq.b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw)) / a0;
    bq.b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosw - 2.0f * sqrtA * alpha)) / a0;
    bq.a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosw)) / a0;
    bq.a2 = ((A + 1.0f) + (A - 1.0f) * cosw - 2.0f * sqrtA * alpha) / a0;
}

void FormantShifter::setHighShelf(Biquad& bq, float sampleRate, float freqHz, float gainDb) noexcept {
    freqHz = std::clamp(freqHz, 120.0f, sampleRate * 0.44f);
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPi * freqHz / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float sqrtA = std::sqrt(A);
    const float alpha = sinw * 0.5f * std::sqrt(2.0f);

    const float a0 = (A + 1.0f) - (A - 1.0f) * cosw + 2.0f * sqrtA * alpha;
    bq.b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosw + 2.0f * sqrtA * alpha)) / a0;
    bq.b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw)) / a0;
    bq.b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosw - 2.0f * sqrtA * alpha)) / a0;
    bq.a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosw)) / a0;
    bq.a2 = ((A + 1.0f) - (A - 1.0f) * cosw - 2.0f * sqrtA * alpha) / a0;
}

void FormantShifter::updateCoefficients() noexcept {
    const float ratio = std::pow(2.0f, toneSemitones_ / 12.0f);
    const float norm = toneSemitones_ / 12.0f; // [-1 .. +1]

    // Nominal human vocal formants F1 (520 Hz), F2 (1550 Hz), F3 (2750 Hz)
    const float f1 = 520.0f * ratio;
    const float f2 = 1550.0f * ratio;
    const float f3 = 2750.0f * ratio;

    const float peakGainDb = 5.5f * std::fabs(norm);
    setPeakingEq(formantBands_[0], sampleRate_, f1, 2.2f, peakGainDb);
    setPeakingEq(formantBands_[1], sampleRate_, f2, 2.6f, peakGainDb);
    setPeakingEq(formantBands_[2], sampleRate_, f3, 3.0f, peakGainDb * 0.85f);

    // Complementary spectral tilt so negative tone boosts chest warmth and positive boosts air/presence
    setLowShelf(lowShelf_, sampleRate_, 320.0f, -7.5f * norm);
    setHighShelf(highShelf_, sampleRate_, 1800.0f, 7.5f * norm);
}

float FormantShifter::processSample(float input) noexcept {
    if (std::fabs(toneSemitones_) < 0.05f) {
        return input;
    }
    float y = lowShelf_.process(input);
    y = highShelf_.process(y);
    for (auto& b : formantBands_) {
        y = b.process(y);
    }
    return y * 0.85f;
}

} // namespace vthin

