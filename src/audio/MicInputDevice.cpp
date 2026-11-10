#include "MicInputDevice.h"

#include <algorithm>
#include <cstring>

namespace vthin {

MicInputDevice::MicInputDevice() noexcept {
    enumerateDevices();
}

MicInputDevice::~MicInputDevice() noexcept {
    stop();
}

const std::vector<MicDeviceInfo>& MicInputDevice::enumerateDevices() {
    devices_.clear();
    devices_.push_back(MicDeviceInfo{WAVE_MAPPER, "Default Windows Mic"});

    const UINT numDevs = waveInGetNumDevs();
    for (UINT i = 0; i < numDevs; ++i) {
        WAVEINCAPSA caps{};
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            caps.szPname[MAXPNAMELEN - 1] = '\0';
            devices_.push_back(MicDeviceInfo{i, std::string(caps.szPname)});
        }
    }
    return devices_;
}

std::string MicInputDevice::getCurrentDeviceName() const {
    if (currentDeviceIndex_ < devices_.size()) {
        return devices_[currentDeviceIndex_].name;
    }
    return "No Microphone";
}

bool MicInputDevice::start(std::size_t listIndex, float sampleRate) {
    stop();
    enumerateDevices();

    if (devices_.empty()) {
        return false;
    }
    if (listIndex >= devices_.size()) {
        listIndex = 0;
    }
    currentDeviceIndex_ = listIndex;
    sampleRate_ = (sampleRate > 8000.0f) ? sampleRate : 44100.0f;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = static_cast<DWORD>(sampleRate_);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>((wfx.nChannels * wfx.wBitsPerSample) / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    stopping_.store(false, std::memory_order_release);
    ringBuffer_.clear();

    const UINT devId = devices_[listIndex].deviceId;
    MMRESULT res = waveInOpen(&hWaveIn_, devId, &wfx,
                              reinterpret_cast<DWORD_PTR>(&MicInputDevice::waveInCallback),
                              reinterpret_cast<DWORD_PTR>(this),
                              CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR || !hWaveIn_) {
        hWaveIn_ = nullptr;
        return false;
    }

    for (std::size_t i = 0; i < kNumBuffers; ++i) {
        std::memset(&headers_[i], 0, sizeof(WAVEHDR));
        std::memset(pcmBuffers_[i], 0, sizeof(pcmBuffers_[i]));
        headers_[i].lpData = reinterpret_cast<LPSTR>(pcmBuffers_[i]);
        headers_[i].dwBufferLength = static_cast<DWORD>(kFramesPerBuffer * sizeof(short));
        waveInPrepareHeader(hWaveIn_, &headers_[i], sizeof(WAVEHDR));
        waveInAddBuffer(hWaveIn_, &headers_[i], sizeof(WAVEHDR));
    }

    res = waveInStart(hWaveIn_);
    if (res != MMSYSERR_NOERROR) {
        stop();
        return false;
    }

    running_.store(true, std::memory_order_release);
    return true;
}

void MicInputDevice::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    if (hWaveIn_) {
        waveInReset(hWaveIn_);
        for (std::size_t i = 0; i < kNumBuffers; ++i) {
            if (headers_[i].dwFlags & WHDR_PREPARED) {
                waveInUnprepareHeader(hWaveIn_, &headers_[i], sizeof(WAVEHDR));
            }
        }
        waveInClose(hWaveIn_);
        hWaveIn_ = nullptr;
    }
    ringBuffer_.clear();
}

void CALLBACK MicInputDevice::waveInCallback(HWAVEIN /*hwi*/, UINT uMsg, DWORD_PTR dwInstance,
                                             DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/) {
    if (uMsg == WIM_DATA && dwInstance != 0) {
        auto* self = reinterpret_cast<MicInputDevice*>(dwInstance);
        auto* hdr = reinterpret_cast<WAVEHDR*>(dwParam1);
        self->handleBufferDone(hdr);
    }
}

void MicInputDevice::handleBufferDone(WAVEHDR* hdr) noexcept {
    if (!hdr || stopping_.load(std::memory_order_acquire)) {
        return;
    }

    const std::size_t numSamples = hdr->dwBytesRecorded / sizeof(short);
    const auto* pcm = reinterpret_cast<const short*>(hdr->lpData);

    float temp[kFramesPerBuffer];
    const std::size_t count = std::min(numSamples, kFramesPerBuffer);
    for (std::size_t i = 0; i < count; ++i) {
        temp[i] = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);
    }
    ringBuffer_.pushBlock(temp, count);

    if (!stopping_.load(std::memory_order_acquire) && hWaveIn_) {
        waveInAddBuffer(hWaveIn_, hdr, sizeof(WAVEHDR));
    }
}

void MicInputDevice::readSamples(float* outMono, std::size_t count) noexcept {
    if (!outMono || count == 0) return;

    if (!running_.load(std::memory_order_relaxed)) {
        std::fill(outMono, outMono + count, 0.0f);
        return;
    }

    // Prevent latency buildup if host pauses or audio clocks drift (>2048 samples ~ 46 ms)
    while (ringBuffer_.availableToRead() > 2048 + count) {
        float discard[256];
        ringBuffer_.popBlock(discard, 256);
    }

    const std::size_t got = ringBuffer_.popBlock(outMono, count);
    if (got > 0) {
        lastSample_ = outMono[got - 1];
    }
    for (std::size_t i = got; i < count; ++i) {
        lastSample_ *= 0.95f;
        outMono[i] = lastSample_;
    }
}

} // namespace vthin

