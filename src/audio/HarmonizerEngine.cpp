#include "HarmonizerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vthin {

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kPi = 3.14159265358979323846f;

float midiNoteToHz(float midiNote) noexcept {
    return 440.0f * std::pow(2.0f, (midiNote - 69.0f) / 12.0f);
}

float polyBlep(float t, float dt) noexcept {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
} // namespace

HarmonizerEngine::HarmonizerEngine() noexcept {
    // Initialize default parameter values per SPEC.md
    params_[kParamDryLevel].store(0.70f, std::memory_order_relaxed);
    params_[kParamHarmonyLevel].store(0.85f, std::memory_order_relaxed);
    params_[kParamVocalTone].store(0.50f, std::memory_order_relaxed);     // 0.0 semitones
    params_[kParamVocoderBlend].store(0.20f, std::memory_order_relaxed);  // 20% vocoder blend
    params_[kParamChoirDetune].store(0.24f, std::memory_order_relaxed);   // 12 cents
    params_[kParamStereoWidth].store(0.75f, std::memory_order_relaxed);   // 75% stereo spread
    params_[kParamPitchMode].store(0.0f, std::memory_order_relaxed);      // Default: Pitch-Tracked Lock Mode
    params_[kParamAttackRelease].store(0.10f, std::memory_order_relaxed); // ~45 ms
    params_[kParamReverbMix].store(0.22f, std::memory_order_relaxed);     // 22% vocal reverb
    params_[kParamNoiseGate].store(0.30f, std::memory_order_relaxed);     // -52 dB gate

    for (auto& b : activeNotesBitmap_) {
        b.store(false, std::memory_order_relaxed);
    }

    prepare(44100.0f);
}

void HarmonizerEngine::prepare(float sampleRate) noexcept {
    sampleRate_ = (sampleRate > 8000.0f) ? sampleRate : 44100.0f;
    pitchDetector_.prepare(sampleRate_);
    formantShifterL_.prepare(sampleRate_);
    formantShifterR_.prepare(sampleRate_);
    vocoderL_.prepare(sampleRate_);
    vocoderR_.prepare(sampleRate_);

    for (std::size_t i = 0; i < kMaxVoices; ++i) {
        voices_[i].shifter.prepare(sampleRate_);
        voices_[i].lfoPhase = static_cast<float>(i) / static_cast<float>(kMaxVoices);
    }

    // Stereo Schroeder/Moorer reverb delay lines scaled to sampleRate
    const float scale = sampleRate_ / 44100.0f;
    constexpr std::size_t kCombLengths[4] = {1116, 1188, 1277, 1356};
    constexpr std::size_t kAllpassLengths[2] = {556, 441};
    for (int i = 0; i < 4; ++i) {
        combsL_[i].init(static_cast<std::size_t>(kCombLengths[i] * scale) + 1);
        combsR_[i].init(static_cast<std::size_t>((kCombLengths[i] + 23) * scale) + 1);
    }
    for (int i = 0; i < 2; ++i) {
        allpassL_[i].init(static_cast<std::size_t>(kAllpassLengths[i] * scale) + 1);
        allpassR_[i].init(static_cast<std::size_t>((kAllpassLengths[i] + 19) * scale) + 1);
    }

    reset();
}

void HarmonizerEngine::reset() noexcept {
    pitchDetector_.reset();
    formantShifterL_.reset();
    formantShifterR_.reset();
    vocoderL_.reset();
    vocoderR_.reset();

    for (auto& v : voices_) {
        v.active = false;
        v.releasing = false;
        v.envelope = 0.0f;
        v.shifter.reset();
    }
    for (auto& b : activeNotesBitmap_) {
        b.store(false, std::memory_order_relaxed);
    }
    for (int i = 0; i < 4; ++i) {
        combsL_[i].clear();
        combsR_[i].clear();
    }
    for (int i = 0; i < 2; ++i) {
        allpassL_[i].clear();
        allpassR_[i].clear();
    }
    hpfStateX_ = 0.0f;
    hpfStateY_ = 0.0f;
    gateEnv_ = 0.0f;
    gateGain_ = 1.0f;
}

void HarmonizerEngine::setParameterNormalized(int index, float value) noexcept {
    if (index >= 0 && index < kNumParams) {
        params_[index].store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    }
}

float HarmonizerEngine::getParameterNormalized(int index) const noexcept {
    if (index >= 0 && index < kNumParams) {
        return params_[index].load(std::memory_order_relaxed);
    }
    return 0.0f;
}

const char* HarmonizerEngine::getParameterName(int index) noexcept {
    switch (index) {
        case kParamDryLevel:      return "Dry Voice";
        case kParamHarmonyLevel:  return "Harmony";
        case kParamVocalTone:     return "Vocal Tone";
        case kParamVocoderBlend:  return "Vocoder";
        case kParamChoirDetune:   return "Detune";
        case kParamStereoWidth:   return "Stereo Wid";
        case kParamPitchMode:     return "Pitch Mode";
        case kParamAttackRelease: return "Env Smooth";
        case kParamReverbMix:     return "Space Rev";
        case kParamNoiseGate:     return "Noise Gate";
        default:                  return "Param";
    }
}

const char* HarmonizerEngine::getParameterLabel(int index) noexcept {
    switch (index) {
        case kParamDryLevel:
        case kParamHarmonyLevel:
        case kParamVocoderBlend:
        case kParamStereoWidth:
        case kParamReverbMix:
            return "%";
        case kParamVocalTone:
            return "st";
        case kParamChoirDetune:
            return "ct";
        case kParamPitchMode:
            return "";
        case kParamAttackRelease:
            return "ms";
        case kParamNoiseGate:
            return "dB";
        default:
            return "";
    }
}

void HarmonizerEngine::getParameterDisplay(int index, char* outText, std::size_t maxLen) const noexcept {
    if (!outText || maxLen == 0) return;
    const float v = getParameterNormalized(index);
    switch (index) {
        case kParamDryLevel:
        case kParamHarmonyLevel:
        case kParamVocoderBlend:
        case kParamStereoWidth:
        case kParamReverbMix:
            std::snprintf(outText, maxLen, "%.0f", v * 100.0f);
            break;
        case kParamVocalTone:
            std::snprintf(outText, maxLen, "%+.1f", (v - 0.5f) * 24.0f);
            break;
        case kParamChoirDetune:
            std::snprintf(outText, maxLen, "%.1f", v * 50.0f);
            break;
        case kParamPitchMode:
            std::snprintf(outText, maxLen, "%s", (v < 0.5f) ? "MIDI Lock" : "Relative");
            break;
        case kParamAttackRelease:
            std::snprintf(outText, maxLen, "%.0f", 5.0f + v * 395.0f);
            break;
        case kParamNoiseGate:
            std::snprintf(outText, maxLen, "%.1f", -70.0f + v * 55.0f);
            break;
        default:
            std::snprintf(outText, maxLen, "%.2f", v);
            break;
    }
}

void HarmonizerEngine::noteOn(int midiNote, float velocity) noexcept {
    midiNote = std::clamp(midiNote, 0, 127);
    if (velocity <= 0.001f) {
        noteOff(midiNote);
        return;
    }
    activeNotesBitmap_[midiNote].store(true, std::memory_order_relaxed);

    // 1. Retrigger existing voice if same note
    for (auto& v : voices_) {
        if (v.active && v.midiNote == midiNote) {
            v.releasing = false;
            v.velocity = std::clamp(velocity, 0.1f, 1.0f);
            v.age = ++voiceCounter_;
            return;
        }
    }

    // 2. Find inactive voice
    for (auto& v : voices_) {
        if (!v.active) {
            v.active = true;
            v.releasing = false;
            v.midiNote = midiNote;
            v.velocity = std::clamp(velocity, 0.1f, 1.0f);
            v.envelope = 0.0f;
            v.age = ++voiceCounter_;
            v.shifter.reset();
            return;
        }
    }

    // 3. Steal oldest releasing voice (or oldest active voice)
    Voice* victim = &voices_[0];
    for (auto& v : voices_) {
        if (v.releasing && (!victim->releasing || v.age < victim->age)) {
            victim = &v;
        } else if (!victim->releasing && v.age < victim->age) {
            victim = &v;
        }
    }
    victim->active = true;
    victim->releasing = false;
    victim->midiNote = midiNote;
    victim->velocity = std::clamp(velocity, 0.1f, 1.0f);
    victim->age = ++voiceCounter_;
}

void HarmonizerEngine::noteOff(int midiNote) noexcept {
    midiNote = std::clamp(midiNote, 0, 127);
    activeNotesBitmap_[midiNote].store(false, std::memory_order_relaxed);
    for (auto& v : voices_) {
        if (v.active && v.midiNote == midiNote) {
            v.releasing = true;
        }
    }
}

void HarmonizerEngine::allNotesOff() noexcept {
    for (auto& b : activeNotesBitmap_) {
        b.store(false, std::memory_order_relaxed);
    }
    for (auto& v : voices_) {
        v.releasing = true;
    }
}

void HarmonizerEngine::setPitchBendSemitones(float semitones) noexcept {
    pitchBendSemitones_ = std::clamp(semitones, -12.0f, 12.0f);
}

bool HarmonizerEngine::isNoteActive(int midiNote) const noexcept {
    if (midiNote < 0 || midiNote >= 128) return false;
    return activeNotesBitmap_[midiNote].load(std::memory_order_relaxed);
}

int HarmonizerEngine::getActiveVoiceCount() const noexcept {
    int count = 0;
    for (const auto& v : voices_) {
        if (v.active && !v.releasing) {
            ++count;
        }
    }
    return count;
}

float HarmonizerEngine::generateTestVocalSample() noexcept {
    // Rich synthetic human vowel ("Ah" / "Oo") at 220 Hz (A3) with natural 5.5 Hz vibrato
    testVibPhase_ += 5.5f / sampleRate_;
    if (testVibPhase_ >= 1.0f) testVibPhase_ -= 1.0f;
    const float vibrato = 1.0f + 0.008f * std::sin(kTwoPi * testVibPhase_);
    const float f0 = 220.0f * vibrato;
    const float dt = f0 / sampleRate_;

    testPhase_ += dt;
    if (testPhase_ >= 1.0f) testPhase_ -= 1.0f;

    // Band-limited glottal pulse train with formant-rich harmonics
    const float p = kTwoPi * testPhase_;
    const float glottal = 0.45f * std::sin(p)
                        + 0.30f * std::sin(2.0f * p)
                        + 0.22f * std::sin(3.0f * p)
                        + 0.16f * std::sin(4.0f * p)
                        + 0.12f * std::sin(5.0f * p)
                        + 0.08f * std::sin(7.0f * p)
                        + 0.05f * std::sin(10.0f * p);

    // Subtle breath aspiration noise
    rngState_ = rngState_ * 1664525u + 1013904223u;
    const float noise = (static_cast<float>(rngState_ & 0xFFFFu) / 32768.0f - 1.0f) * 0.015f;
    return (glottal + noise) * 0.55f;
}

void HarmonizerEngine::processReverb(float inL, float inR, float& outL, float& outR) noexcept {
    float sumL = 0.0f;
    float sumR = 0.0f;
    const float revInL = inL * 0.25f;
    const float revInR = inR * 0.25f;

    for (int i = 0; i < 4; ++i) {
        sumL += combsL_[i].process(revInL);
        sumR += combsR_[i].process(revInR);
    }
    for (int i = 0; i < 2; ++i) {
        sumL = allpassL_[i].process(sumL);
        sumR = allpassR_[i].process(sumR);
    }
    outL = sumL;
    outR = sumR;
}

void HarmonizerEngine::processBlock(const float* inL, const float* inR,
                                    const float* micSamples,
                                    float* outL, float* outR,
                                    std::size_t numSamples) noexcept {
    const float dryGain = params_[kParamDryLevel].load(std::memory_order_relaxed);
    const float harmonyGain = params_[kParamHarmonyLevel].load(std::memory_order_relaxed);
    const float toneSemitones = (params_[kParamVocalTone].load(std::memory_order_relaxed) - 0.5f) * 24.0f;
    const float vocoderBlend = params_[kParamVocoderBlend].load(std::memory_order_relaxed);
    const float detuneCents = params_[kParamChoirDetune].load(std::memory_order_relaxed) * 50.0f;
    const float stereoWidth = params_[kParamStereoWidth].load(std::memory_order_relaxed);
    const bool pitchLockMode = (params_[kParamPitchMode].load(std::memory_order_relaxed) < 0.5f);
    const float envTimeMs = 5.0f + params_[kParamAttackRelease].load(std::memory_order_relaxed) * 395.0f;
    const float reverbMix = params_[kParamReverbMix].load(std::memory_order_relaxed);
    const float gateDb = -70.0f + params_[kParamNoiseGate].load(std::memory_order_relaxed) * 55.0f;
    const float gateThreshold = (gateDb <= -68.0f) ? 0.0f : std::pow(10.0f, gateDb / 20.0f);

    const float inGainLinear = std::pow(10.0f, inputGainDb_.load(std::memory_order_relaxed) / 20.0f);
    const InputSourceMode srcMode = getInputSourceMode();

    formantShifterL_.setToneSemitones(toneSemitones);
    formantShifterR_.setToneSemitones(toneSemitones);
    vocoderL_.setFormantShiftSemitones(toneSemitones);
    vocoderR_.setFormantShiftSemitones(toneSemitones);

    const float envStep = 1.0f / std::max(1.0f, sampleRate_ * (envTimeMs * 0.001f));
    const float hpfAlpha = sampleRate_ / (sampleRate_ + kTwoPi * 75.0f);

    // Collect indices of currently active voices so we can pan them symmetrically across L/R
    std::array<std::size_t, kMaxVoices> activeIndices{};
    std::size_t numActive = 0;
    for (std::size_t v = 0; v < kMaxVoices; ++v) {
        if (voices_[v].active) {
            activeIndices[numActive++] = v;
        }
    }

    float sumInSq = 0.0f;
    float sumOutSq = 0.0f;

    for (std::size_t i = 0; i < numSamples; ++i) {
        float rawInput = 0.0f;
        const float hostMono = ((inL ? inL[i] : 0.0f) + (inR ? inR[i] : 0.0f)) * 0.5f;
        const float micSample = micSamples ? micSamples[i] : 0.0f;

        if (srcMode == InputSourceMode::kTestVocalGenerator) {
            rawInput = generateTestVocalSample();
        } else if (srcMode == InputSourceMode::kHostAudio) {
            rawInput = hostMono;
        } else {
            // Default kMicrophone: sum live microphone + any host audio present
            rawInput = micSample + hostMono;
        }

        rawInput *= inGainLinear;

        // 75 Hz High-Pass filter to remove mic handling rumble and DC offset
        const float hpfOut = hpfAlpha * (hpfStateY_ + rawInput - hpfStateX_);
        hpfStateX_ = rawInput;
        hpfStateY_ = hpfOut;

        // Smooth noise gate
        const float absIn = std::fabs(hpfOut);
        gateEnv_ = (absIn > gateEnv_) ? (0.02f * absIn + 0.98f * gateEnv_)
                                      : (0.0008f * absIn + 0.9992f * gateEnv_);
        const float targetGate = (gateEnv_ >= gateThreshold) ? 1.0f : 0.0f;
        gateGain_ = 0.005f * targetGate + 0.995f * gateGain_;

        const float cleanVoice = hpfOut * gateGain_;
        sumInSq += cleanVoice * cleanVoice;

        // 1. Update real-time fundamental pitch detector (YIN)
        pitchDetector_.processSample(cleanVoice);
        const float f0 = std::clamp(pitchDetector_.getDetectedPitchHz(), 70.0f, 950.0f);

        // 2. Process all active MIDI voices
        float pitchedSumL = 0.0f;
        float pitchedSumR = 0.0f;
        float carrierSumL = 0.0f;
        float carrierSumR = 0.0f;

        for (std::size_t k = 0; k < numActive; ++k) {
            const std::size_t vIdx = activeIndices[k];
            Voice& v = voices_[vIdx];
            if (!v.active) continue;

            // Update ADSR envelope
            if (!v.releasing) {
                v.envelope = std::min(1.0f, v.envelope + envStep);
            } else {
                v.envelope -= envStep * 0.7f;
                if (v.envelope <= 0.0001f) {
                    v.envelope = 0.0f;
                    v.active = false;
                    v.releasing = false;
                    continue;
                }
            }

            // Per-voice detune spread + gentle LFO humanization
            v.lfoPhase += 4.2f / sampleRate_;
            if (v.lfoPhase >= 1.0f) v.lfoPhase -= 1.0f;
            const float lfoCents = std::sin(kTwoPi * v.lfoPhase) * (detuneCents * 0.25f);
            const float voiceSign = (k & 1u) ? 1.0f : -1.0f;
            const float spreadIndex = static_cast<float>((k + 1) / 2) / static_cast<float>(kMaxVoices / 2);
            const float totalDetuneSemitones = (voiceSign * spreadIndex * detuneCents + lfoCents) * 0.01f;

            const float effectiveMidiNote = static_cast<float>(v.midiNote) + pitchBendSemitones_ + totalDetuneSemitones;
            const float targetHz = midiNoteToHz(effectiveMidiNote);

            float pitchRatio = 1.0f;
            if (pitchLockMode) {
                // Pitch-Tracked Lock Mode: locks shifted voice to exact MIDI note frequency
                pitchRatio = targetHz / f0;
            } else {
                // Relative Semitone Shift Mode relative to Middle C (MIDI 60)
                pitchRatio = std::pow(2.0f, (effectiveMidiNote - 60.0f) / 12.0f);
            }
            v.shifter.setPitchRatio(pitchRatio);

            const float shiftedVoice = v.shifter.processSample(cleanVoice) * v.envelope * v.velocity;

            // Synthesize rich band-limited PolyBLEP Saw + Sub-Octave Carrier for the 16-Band Vocoder
            const float dt = std::clamp(targetHz / sampleRate_, 0.0005f, 0.45f);
            v.carrierPhase += dt;
            if (v.carrierPhase >= 1.0f) v.carrierPhase -= 1.0f;
            const float saw = (2.0f * v.carrierPhase - 1.0f) - polyBlep(v.carrierPhase, dt);

            v.subPhase += dt * 0.5f;
            if (v.subPhase >= 1.0f) v.subPhase -= 1.0f;
            const float subPulse = (v.subPhase < 0.5f ? 0.35f : -0.35f);
            const float carrierSample = (saw * 0.75f + subPulse * 0.25f) * v.envelope * v.velocity;

            // Compute stereo pan position [-1.0 (Left) .. +1.0 (Right)]
            float pan = 0.0f;
            if (numActive == 1) {
                // Single voice: subtle stereo chorus motion when stereoWidth > 0
                pan = std::sin(kTwoPi * v.lfoPhase) * 0.35f * stereoWidth;
            } else {
                // Multiple chord notes: spread evenly across [-stereoWidth .. +stereoWidth]
                const float normPos = (2.0f * static_cast<float>(k) / static_cast<float>(numActive - 1)) - 1.0f;
                pan = normPos * stereoWidth;
            }

            const float angle = (pan + 1.0f) * (kPi * 0.25f);
            const float panL = std::cos(angle);
            const float panR = std::sin(angle);

            pitchedSumL += shiftedVoice * panL;
            pitchedSumR += shiftedVoice * panR;
            carrierSumL += carrierSample * panL;
            carrierSumR += carrierSample * panR;
        }

        // Normalize chord headroom gently as more voices are added
        const float voiceNorm = (numActive > 1) ? (1.0f / std::sqrt(static_cast<float>(numActive))) : 1.0f;
        pitchedSumL *= voiceNorm;
        pitchedSumR *= voiceNorm;
        carrierSumL *= voiceNorm;
        carrierSumR *= voiceNorm;

        // 3. Apply Vocal Tone / Formant Shift to the pitch-shifted vocal harmony
        const float formantL = formantShifterL_.processSample(pitchedSumL);
        const float formantR = formantShifterR_.processSample(pitchedSumR);

        // 4. Process 16-band Vocoder and crossfade via Vocoder Blend
        float harmonyL = formantL;
        float harmonyR = formantR;
        if (vocoderBlend > 0.005f) {
            const float vocL = vocoderL_.processSample(cleanVoice, carrierSumL);
            const float vocR = vocoderR_.processSample(cleanVoice, carrierSumR);
            harmonyL = (1.0f - vocoderBlend) * formantL + vocoderBlend * vocL;
            harmonyR = (1.0f - vocoderBlend) * formantR + vocoderBlend * vocR;
        }

        // 5. Apply stereo vocal reverb to harmonized signal
        float revL = 0.0f;
        float revR = 0.0f;
        if (reverbMix > 0.005f) {
            processReverb(harmonyL, harmonyR, revL, revR);
        }
        const float wetL = (harmonyL * (1.0f - reverbMix * 0.35f) + revL * reverbMix) * harmonyGain;
        const float wetR = (harmonyR * (1.0f - reverbMix * 0.35f) + revR * reverbMix) * harmonyGain;

        // 6. Mix Dry Voice + Harmonized Vocals with soft saturation protection
        const float drySample = cleanVoice * dryGain;
        const float finalL = std::tanh(drySample + wetL);
        const float finalR = std::tanh(drySample + wetR);

        outL[i] = finalL;
        outR[i] = finalR;

        const float outMono = 0.5f * (finalL + finalR);
        sumOutSq += outMono * outMono;

        // Push to lock-free oscilloscope buffer for 60 FPS wavelength UI
        scopeBuffer_.pushSample(cleanVoice, outMono);
    }

    detectedPitchHz_.store(pitchDetector_.getDetectedPitchHz(), std::memory_order_relaxed);
    if (numSamples > 0) {
        const float inRms = std::sqrt(sumInSq / static_cast<float>(numSamples));
        const float outRms = std::sqrt(sumOutSq / static_cast<float>(numSamples));
        inputLevelRms_.store(inRms, std::memory_order_relaxed);
        outputLevelRms_.store(outRms, std::memory_order_relaxed);
    }
}

} // namespace vthin

