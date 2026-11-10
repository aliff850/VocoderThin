#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <algorithm>

namespace vthin {

/**
 * Lock-free Single-Producer Single-Consumer (SPSC) ring buffer for real-time audio.
 * Capacity must be a power of two.
 */
template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    RingBuffer() noexcept {
        buffer_.fill(T{});
    }

    void clear() noexcept {
        writeIndex_.store(0, std::memory_order_relaxed);
        readIndex_.store(0, std::memory_order_relaxed);
    }

    std::size_t availableToRead() const noexcept {
        const std::size_t w = writeIndex_.load(std::memory_order_acquire);
        const std::size_t r = readIndex_.load(std::memory_order_relaxed);
        return w - r;
    }

    std::size_t availableToWrite() const noexcept {
        const std::size_t w = writeIndex_.load(std::memory_order_relaxed);
        const std::size_t r = readIndex_.load(std::memory_order_acquire);
        return Capacity - (w - r);
    }

    bool push(const T& sample) noexcept {
        const std::size_t w = writeIndex_.load(std::memory_order_relaxed);
        const std::size_t r = readIndex_.load(std::memory_order_acquire);
        if (w - r >= Capacity) {
            return false; // full
        }
        buffer_[w & kMask] = sample;
        writeIndex_.store(w + 1, std::memory_order_release);
        return true;
    }

    std::size_t pushBlock(const T* data, std::size_t count) noexcept {
        const std::size_t avail = availableToWrite();
        const std::size_t toWrite = std::min(count, avail);
        std::size_t w = writeIndex_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < toWrite; ++i) {
            buffer_[(w + i) & kMask] = data[i];
        }
        writeIndex_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    bool pop(T& outSample) noexcept {
        const std::size_t r = readIndex_.load(std::memory_order_relaxed);
        const std::size_t w = writeIndex_.load(std::memory_order_acquire);
        if (w == r) {
            return false; // empty
        }
        outSample = buffer_[r & kMask];
        readIndex_.store(r + 1, std::memory_order_release);
        return true;
    }

    std::size_t popBlock(T* outData, std::size_t count) noexcept {
        const std::size_t avail = availableToRead();
        const std::size_t toRead = std::min(count, avail);
        std::size_t r = readIndex_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < toRead; ++i) {
            outData[i] = buffer_[(r + i) & kMask];
        }
        readIndex_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> writeIndex_{0};
    alignas(64) std::atomic<std::size_t> readIndex_{0};
};

/**
 * Lock-free rolling oscilloscope history buffer for visualizing live input (mic)
 * and output (harmonized) wavelengths at 60 FPS without blocking the audio thread.
 */
template <std::size_t Size = 2048>
class ScopeBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");

public:
    ScopeBuffer() noexcept {
        inWave_.fill(0.0f);
        outWave_.fill(0.0f);
    }

    void pushSample(float inSample, float outSample) noexcept {
        const std::size_t pos = writePos_.load(std::memory_order_relaxed);
        inWave_[pos & kMask] = inSample;
        outWave_[pos & kMask] = outSample;
        writePos_.store(pos + 1, std::memory_order_release);
    }

    void copyLatest(float* dstIn, float* dstOut, std::size_t numSamples) const noexcept {
        const std::size_t count = std::min(numSamples, Size);
        const std::size_t endPos = writePos_.load(std::memory_order_acquire);
        const std::size_t startPos = endPos - count;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t idx = (startPos + i) & kMask;
            dstIn[i] = inWave_[idx];
            dstOut[i] = outWave_[idx];
        }
    }

private:
    static constexpr std::size_t kMask = Size - 1;
    std::array<float, Size> inWave_{};
    std::array<float, Size> outWave_{};
    std::atomic<std::size_t> writePos_{0};
};

} // namespace vthin

