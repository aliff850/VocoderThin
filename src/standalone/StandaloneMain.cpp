#include "../vst/PluginProcessor.h"
#include "../ui/PluginEditorWin32.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

namespace {

constexpr float kSampleRate = 44100.0f;
constexpr std::size_t kBufferFrames = 512;
constexpr std::size_t kNumOutBuffers = 6;

vthin::PluginProcessor* gProcessor = nullptr;

intptr_t VTHIN_VST_CALL standaloneHostCallback(vthin::AEffect* /*effect*/, int32_t opcode,
                                               int32_t /*index*/, intptr_t /*value*/,
                                               void* /*ptr*/, float /*opt*/) {
    if (opcode == vthin::audioMasterVersion) {
        return 2400;
    }
    return 0;
}

void CALLBACK midiInCallback(HMIDIIN /*hMidiIn*/, UINT wMsg, DWORD_PTR /*dwInstance*/,
                             DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/) {
    if (wMsg == MIM_DATA && gProcessor) {
        const unsigned char status = static_cast<unsigned char>(dwParam1 & 0xFFu);
        const unsigned char data1  = static_cast<unsigned char>((dwParam1 >> 8) & 0xFFu);
        const unsigned char data2  = static_cast<unsigned char>((dwParam1 >> 16) & 0xFFu);
        gProcessor->processMidiMessage(status, data1, data2);
    }
}

int keyToMidiNote(WPARAM vk) noexcept {
    switch (vk) {
        case 'A': return 60; // C4
        case 'W': return 61; // C#4
        case 'S': return 62; // D4
        case 'E': return 63; // D#4
        case 'D': return 64; // E4
        case 'F': return 65; // F4
        case 'T': return 66; // F#4
        case 'G': return 67; // G4
        case 'Y': return 68; // G#4
        case 'H': return 69; // A4
        case 'U': return 70; // A#4
        case 'J': return 71; // B4
        case 'K': return 72; // C5
        default:  return -1;
    }
}

bool saveWindowBitmapToBmp(HWND hwnd, const char* filePath) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC hdcScreen = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ oldObj = SelectObject(hdcMem, hbm);

    SendMessageA(hwnd, WM_PAINT, 0, 0);
    BitBlt(hdcMem, 0, 0, w, h, hdcScreen, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    const DWORD rowStride = ((w * 24 + 31) / 32) * 4;
    const DWORD imageSize = rowStride * static_cast<DWORD>(h);
    std::vector<unsigned char> pixels(imageSize, 0);

    GetDIBits(hdcMem, hbm, 0, static_cast<UINT>(h), pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + imageSize;

    FILE* fp = std::fopen(filePath, "wb");
    bool ok = false;
    if (fp) {
        std::fwrite(&bfh, sizeof(bfh), 1, fp);
        std::fwrite(&bi, sizeof(bi), 1, fp);
        std::fwrite(pixels.data(), 1, pixels.size(), fp);
        std::fclose(fp);
        ok = true;
    }

    SelectObject(hdcMem, oldObj);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcScreen);
    return ok;
}

LRESULT CALLBACK hostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            if ((lParam & (1 << 30)) == 0 && gProcessor) {
                const int note = keyToMidiNote(wParam);
                if (note >= 0) {
                    gProcessor->processMidiMessage(0x90, static_cast<unsigned char>(note), 110);
                }
            }
            return 0;

        case WM_KEYUP:
            if (gProcessor) {
                const int note = keyToMidiNote(wParam);
                if (note >= 0) {
                    gProcessor->processMidiMessage(0x80, static_cast<unsigned char>(note), 0);
                }
            }
            return 0;

        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow) {
    std::string screenshotPath;
    if (lpCmdLine && std::strstr(lpCmdLine, "--screenshot")) {
        const char* p = std::strstr(lpCmdLine, "--screenshot") + 12;
        while (*p == ' ' || *p == '\"') ++p;
        screenshotPath = p;
        while (!screenshotPath.empty() &&
               (screenshotPath.back() == ' ' || screenshotPath.back() == '\"')) {
            screenshotPath.pop_back();
        }
    }

    vthin::PluginProcessor processor(&standaloneHostCallback);
    gProcessor = &processor;
    vthin::AEffect* effect = processor.getAEffect();

    effect->dispatcher(effect, vthin::effOpen, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, vthin::effSetSampleRate, 0, 0, nullptr, kSampleRate);
    effect->dispatcher(effect, vthin::effSetBlockSize, 0, static_cast<intptr_t>(kBufferFrames), nullptr, 0.0f);
    effect->dispatcher(effect, vthin::effMainsChanged, 0, 1, nullptr, 0.0f);

    // Register host container window
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &hostWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "VocoderThinStandaloneHostWnd";
    RegisterClassExA(&wc);

    RECT winRect{0, 0, vthin::PluginEditorWin32::kWidth, vthin::PluginEditorWin32::kHeight};
    const DWORD winStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&winRect, winStyle, FALSE);

    HWND hostHwnd = CreateWindowExA(
        0, wc.lpszClassName,
        "VocoderThin - MIDI Vocal Harmonizer & Vocoder (Standalone)",
        winStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        winRect.right - winRect.left, winRect.bottom - winRect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hostHwnd) {
        return 1;
    }

    effect->dispatcher(effect, vthin::effEditOpen, 0, 0, hostHwnd, 0.0f);
    ShowWindow(hostHwnd, nCmdShow);
    UpdateWindow(hostHwnd);

    // If running in automated screenshot capture mode, populate live chord & test voice wave, capture BMP, and exit
    if (!screenshotPath.empty()) {
        processor.getEngine().setInputSourceMode(vthin::InputSourceMode::kTestVocalGenerator);
        processor.processMidiMessage(0x90, 60, 110); // C4
        processor.processMidiMessage(0x90, 64, 110); // E4
        processor.processMidiMessage(0x90, 67, 110); // G4
        processor.processMidiMessage(0x90, 71, 100); // B4

        std::vector<float> inL(kBufferFrames, 0.0f);
        std::vector<float> inR(kBufferFrames, 0.0f);
        std::vector<float> outL(kBufferFrames, 0.0f);
        std::vector<float> outR(kBufferFrames, 0.0f);
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        for (int b = 0; b < 36; ++b) {
            effect->processReplacing(effect, ins, outs, static_cast<int32_t>(kBufferFrames));
        }

        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        HWND childEditor = GetWindow(hostHwnd, GW_CHILD);
        saveWindowBitmapToBmp(childEditor ? childEditor : hostHwnd, screenshotPath.c_str());

        effect->dispatcher(effect, vthin::effEditClose, 0, 0, nullptr, 0.0f);
        DestroyWindow(hostHwnd);
        gProcessor = nullptr;
        return 0;
    }

    // Open all available Windows MIDI input controllers
    std::vector<HMIDIIN> midiHandles;
    const UINT numMidiDevs = midiInGetNumDevs();
    for (UINT i = 0; i < numMidiDevs; ++i) {
        HMIDIIN hMidi = nullptr;
        if (midiInOpen(&hMidi, i, reinterpret_cast<DWORD_PTR>(&midiInCallback), 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            midiInStart(hMidi);
            midiHandles.push_back(hMidi);
        }
    }

    // Open Windows waveOut stereo audio stream
    HWAVEOUT hWaveOut = nullptr;
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = static_cast<DWORD>(kSampleRate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    std::atomic<bool> audioRunning{false};
    std::thread audioThread;
    WAVEHDR outHeaders[kNumOutBuffers]{};
    short outPcm[kNumOutBuffers][kBufferFrames * 2]{};

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR && hWaveOut) {
        for (std::size_t i = 0; i < kNumOutBuffers; ++i) {
            outHeaders[i].lpData = reinterpret_cast<LPSTR>(outPcm[i]);
            outHeaders[i].dwBufferLength = static_cast<DWORD>(kBufferFrames * 2 * sizeof(short));
            waveOutPrepareHeader(hWaveOut, &outHeaders[i], sizeof(WAVEHDR));
            outHeaders[i].dwFlags |= WHDR_DONE;
        }

        audioRunning.store(true);
        audioThread = std::thread([&]() {
            std::vector<float> inL(kBufferFrames, 0.0f);
            std::vector<float> inR(kBufferFrames, 0.0f);
            std::vector<float> outL(kBufferFrames, 0.0f);
            std::vector<float> outR(kBufferFrames, 0.0f);
            float* ins[2] = {inL.data(), inR.data()};
            float* outs[2] = {outL.data(), outR.data()};

            while (audioRunning.load(std::memory_order_relaxed)) {
                bool filledAny = false;
                for (std::size_t i = 0; i < kNumOutBuffers; ++i) {
                    if (outHeaders[i].dwFlags & WHDR_DONE) {
                        effect->processReplacing(effect, ins, outs, static_cast<int32_t>(kBufferFrames));
                        for (std::size_t f = 0; f < kBufferFrames; ++f) {
                            const float l = std::clamp(outL[f], -1.0f, 1.0f);
                            const float r = std::clamp(outR[f], -1.0f, 1.0f);
                            outPcm[i][f * 2]     = static_cast<short>(l * 32767.0f);
                            outPcm[i][f * 2 + 1] = static_cast<short>(r * 32767.0f);
                        }
                        waveOutWrite(hWaveOut, &outHeaders[i], sizeof(WAVEHDR));
                        filledAny = true;
                    }
                }
                if (!filledAny) {
                    Sleep(2);
                }
            }
        });
    }

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    audioRunning.store(false);
    if (audioThread.joinable()) {
        audioThread.join();
    }
    if (hWaveOut) {
        waveOutReset(hWaveOut);
        for (std::size_t i = 0; i < kNumOutBuffers; ++i) {
            waveOutUnprepareHeader(hWaveOut, &outHeaders[i], sizeof(WAVEHDR));
        }
        waveOutClose(hWaveOut);
    }
    for (HMIDIIN hMidi : midiHandles) {
        midiInStop(hMidi);
        midiInClose(hMidi);
    }

    effect->dispatcher(effect, vthin::effEditClose, 0, 0, nullptr, 0.0f);
    gProcessor = nullptr;
    return 0;
}

