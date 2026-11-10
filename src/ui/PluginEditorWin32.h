#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vthin {

class PluginProcessor;

/**
 * 60 FPS double-buffered Win32 custom GUI editor for VocoderThin.
 * Renders live input/output wavelengths (oscilloscope), pitch/chord telemetry,
 * microphone selector, 9 rotary parameter knobs, and a 25-key MIDI keyboard.
 */
class PluginEditorWin32 {
public:
    static constexpr int kWidth = 820;
    static constexpr int kHeight = 520;

    explicit PluginEditorWin32(PluginProcessor& processor) noexcept;
    ~PluginEditorWin32() noexcept;

    PluginEditorWin32(const PluginEditorWin32&) = delete;
    PluginEditorWin32& operator=(const PluginEditorWin32&) = delete;

    bool open(HWND parentHwnd) noexcept;
    void close() noexcept;
    void idle() noexcept;

    HWND getHwnd() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void paint(HDC hdc);
    void drawHeaderBar(HDC hdc);
    void drawOscilloscope(HDC hdc);
    void drawKnobs(HDC hdc);
    void drawKeyboard(HDC hdc);

    int hitTestKnob(int x, int y) const noexcept;
    int hitTestPianoKey(int x, int y) const noexcept;
    void onMouseDown(int x, int y, bool rightButton);
    void onMouseMove(int x, int y);
    void onMouseUp();

    PluginProcessor& processor_;
    HWND hwnd_{nullptr};
    UINT_PTR timerId_{0};

    // Double-buffer backbuffer
    HDC memDc_{nullptr};
    HBITMAP memBitmap_{nullptr};
    HBITMAP oldBitmap_{nullptr};

    // Interaction state
    int draggingKnob_{-1};
    int dragStartY_{0};
    float dragStartVal_{0.0f};
    int heldMouseKey_{-1};
};

} // namespace vthin

