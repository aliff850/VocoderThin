#pragma once

#include "RingBuffer.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

namespace vthin {

struct MicDeviceInfo {
    UINT deviceId{WAVE_MAPPER};
    std::string name{"Default Windows Microphone"};
};

/**
 * Low-latency Windows Multimedia (waveIn) microphone capture manager.
 * Streams live microphone audio into a lock-free SPSC RingBuffer for the VST audio thread.
 */
class MicInputDevice {
public:
    static constexpr std::size_t kNumBuffers = 8;
    static constexpr std::size_t kFramesPerBuffer = 256;

    MicInputDevice() noexcept;
    ~MicInputDevice() noexcept;

    MicInputDevice(const MicInputDevice&) = delete;
    MicInputDevice& operator=(const MicInputDevice&) = delete;

    /**
     * Refresh and return available microphone input devices.
     * Index 0 is always "Default Windows Mic (WaveMapper)".
     */
    const std::vector<MicDeviceInfo>& enumerateDevices();

    /**
     * Start capturing from the device at listIndex (into enumerateDevices()) at sampleRate.
     */
    bool start(std::size_t listIndex, float sampleRate);

    /**
     * Stop microphone capture and release Windows audio handle.
     */
    void stop() noexcept;

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    std::size_t getCurrentDeviceIndex() const noexcept { return currentDeviceIndex_; }
    std::string getCurrentDeviceName() const;

    /**
     * Read up to count mono samples on the real-time audio thread (lock-free).
     */
    void readSamples(float* outMono, std::size_t count) noexcept;

private:
    static void CALLBACK waveInCallback(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                                        DWORD_PTR dwParam1, DWORD_PTR dwParam2);
    void handleBufferDone(WAVEHDR* hdr) noexcept;

    std::vector<MicDeviceInfo> devices_{};
    std::size_t currentDeviceIndex_{0};
    float sampleRate_{44100.0f};

    HWAVEIN hWaveIn_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    WAVEHDR headers_[kNumBuffers]{};
    short pcmBuffers_[kNumBuffers][kFramesPerBuffer]{};

    RingBuffer<float, 16384> ringBuffer_{};
    float lastSample_{0.0f};
};

} // namespace vthin

