#include "VocoderBank.h"

#include <algorithm>
#include <cmath>

namespace vthin {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

void VocoderBank::BiquadBandpass::configure(float sampleRate, float centerHz, float q) noexcept {
    centerHz = std::clamp(centerHz, 70.0f, sampleRate * 0.43f);
    const float w0 = 2.0f * kPi * centerHz / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.0f * q);

    const float a0 = 1.0f + alpha;
    b0 = (alpha) / a0;
    b1 = 0.0f;
    b2 = (-alpha) / a0;
    a1 = (-2.0f * cosw) / a0;
    a2 = (1.0f - alpha) / a0;
}

VocoderBank::VocoderBank() noexcept {
    prepare(44100.0f);
}

void VocoderBank::prepare(float sampleRate) noexcept {
    sampleRate_ = (sampleRate > 8000.0f) ? sampleRate : 44100.0f;
    // Fast 3 ms attack, smooth 25 ms release for vocal syllabic articulation
    attackCoeff_ = 1.0f - std::exp(-1.0f / (sampleRate_ * 0.003f));
    releaseCoeff_ = 1.0f - std::exp(-1.0f / (sampleRate_ * 0.025f));
    reset();
    updateBands();
}

void VocoderBank::reset() noexcept {
    for (std::size_t i = 0; i < kNumBands; ++i) {
        modFilters_[i].reset();
        carFilters_[i].reset();
        envelopes_[i] = 0.0f;
    }
    sibilanceState_ = 0.0f;
}

void VocoderBank::setFormantShiftSemitones(float semitones) noexcept {
    const float clamped = std::clamp(semitones, -12.0f, 12.0f);
    if (std::fabs(clamped - formantSemitones_) > 0.05f) {
        formantSemitones_ = clamped;
        updateBands();
    }
}

void VocoderBank::updateBands() noexcept {
    constexpr float kMinHz = 110.0f;
    constexpr float kMaxHz = 6800.0f;
    const float formantRatio = std::pow(2.0f, formantSemitones_ / 12.0f);

    for (std::size_t i = 0; i < kNumBands; ++i) {
        const float frac = static_cast<float>(i) / static_cast<float>(kNumBands - 1);
        const float centerHz = kMinHz * std::pow(kMaxHz / kMinHz, frac);
        const float shiftedHz = centerHz * formantRatio;
        constexpr float kBandQ = 4.2f;

        modFilters_[i].configure(sampleRate_, centerHz, kBandQ);
        carFilters_[i].configure(sampleRate_, shiftedHz, kBandQ);
    }
}

float VocoderBank::processSample(float modulatorVoice, float carrierSynth) noexcept {
    float sum = 0.0f;
    for (std::size_t i = 0; i < kNumBands; ++i) {
        const float modBand = modFilters_[i].process(modulatorVoice);
        const float absMod = std::fabs(modBand);
        float env = envelopes_[i];
        if (absMod > env) {
            env += attackCoeff_ * (absMod - env);
        } else {
            env += releaseCoeff_ * (absMod - env);
        }
        envelopes_[i] = env;

        const float carBand = carFilters_[i].process(carrierSynth);
        sum += carBand * env;
    }

    // Add gentle high-frequency sibilance (>4.5 kHz) for crisp vocal diction
    const float highPass = modulatorVoice - sibilanceState_;
    sibilanceState_ += 0.45f * highPass;

    return sum * 4.8f + highPass * 0.12f;
}

} // namespace vthin

