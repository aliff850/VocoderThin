#include "PluginEditorWin32.h"
#include "../vst/PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <windowsx.h>

namespace vthin {

namespace {
constexpr const char* kWindowClassName = "VocoderThinEditorWndClass";
constexpr UINT_PTR kTimerId60Fps = 101;
constexpr float kPi = 3.14159265358979323846f;

// Studio color palette (Win32 COLORREF = 0x00BBGGRR)
constexpr COLORREF kColBgMain       = RGB(18, 20, 28);
constexpr COLORREF kColBgPanel      = RGB(26, 30, 42);
constexpr COLORREF kColBgHeader     = RGB(22, 25, 36);
constexpr COLORREF kColBorder       = RGB(52, 60, 82);
constexpr COLORREF kColGrid         = RGB(35, 42, 58);
constexpr COLORREF kColCyanWave     = RGB(0, 229, 255);
constexpr COLORREF kColMagentaWave  = RGB(255, 75, 145);
constexpr COLORREF kColAccentAmber  = RGB(255, 190, 60);
constexpr COLORREF kColTextBright   = RGB(238, 242, 252);
constexpr COLORREF kColTextDim      = RGB(148, 158, 182);
constexpr COLORREF kColBtnBg        = RGB(36, 43, 62);
constexpr COLORREF kColBtnActive    = RGB(0, 120, 155);

// 9 primary rotary knobs displayed in the knob bank (indices into ParamId)
constexpr int kDisplayedKnobs[9] = {
    kParamDryLevel,
    kParamHarmonyLevel,
    kParamVocalTone,
    kParamVocoderBlend,
    kParamChoirDetune,
    kParamStereoWidth,
    kParamAttackRelease,
    kParamReverbMix,
    kParamNoiseGate
};

constexpr float kDefaultKnobValues[kNumParams] = {
    0.70f, 0.85f, 0.50f, 0.20f, 0.24f, 0.75f, 0.0f, 0.10f, 0.22f, 0.30f
};

// Header button rectangles
const RECT kRectBtnSource = {236, 11, 366, 41};
const RECT kRectBtnMicDev = {372, 11, 526, 41};
const RECT kRectBtnMode   = {532, 11, 662, 41};
const RECT kRectBtnPreset = {668, 11, 808, 41};
const RECT kRectBtnPanic  = {700, 396, 804, 416};

bool isBlackKey(int midiNote) noexcept {
    const int n = midiNote % 12;
    return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
}

void formatFrequencyAndNote(float hz, char* outBuf, std::size_t maxLen) noexcept {
    static const char* kNoteNames[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    if (hz < 65.0f || hz > 1000.0f) {
        std::snprintf(outBuf, maxLen, "--- Hz");
        return;
    }
    const float midiFloat = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const int midiNearest = std::clamp(static_cast<int>(std::round(midiFloat)), 0, 127);
    const int cents = static_cast<int>(std::round((midiFloat - static_cast<float>(midiNearest)) * 100.0f));
    const int octave = (midiNearest / 12) - 1;
    const char* name = kNoteNames[midiNearest % 12];
    std::snprintf(outBuf, maxLen, "%.1f Hz (%s%d %+dc)", hz, name, octave, cents);
}

void fillRectColor(HDC hdc, const RECT& r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(hdc, &r, br);
    DeleteObject(br);
}

void frameRectColor(HDC hdc, const RECT& r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FrameRect(hdc, &r, br);
    DeleteObject(br);
}
} // namespace

PluginEditorWin32::PluginEditorWin32(PluginProcessor& processor) noexcept
    : processor_(processor) {}

PluginEditorWin32::~PluginEditorWin32() noexcept {
    close();
}

bool PluginEditorWin32::open(HWND parentHwnd) noexcept {
    close();

    HINSTANCE hInst = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    if (!GetClassInfoExA(hInst, kWindowClassName, &wc)) {
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &PluginEditorWin32::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClassName;
        RegisterClassExA(&wc);
    }

    const DWORD style = parentHwnd ? (WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN)
                                   : (WS_POPUP | WS_VISIBLE);
    hwnd_ = CreateWindowExA(0, kWindowClassName, "VocoderThin Editor", style,
                            0, 0, kWidth, kHeight,
                            parentHwnd, nullptr, hInst, this);
    if (!hwnd_) {
        return false;
    }

    HDC screenDc = GetDC(hwnd_);
    memDc_ = CreateCompatibleDC(screenDc);
    memBitmap_ = CreateCompatibleBitmap(screenDc, kWidth, kHeight);
    oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, memBitmap_));
    ReleaseDC(hwnd_, screenDc);

    timerId_ = SetTimer(hwnd_, kTimerId60Fps, 16, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void PluginEditorWin32::close() noexcept {
    if (heldMouseKey_ >= 0) {
        processor_.getEngine().noteOff(heldMouseKey_);
        heldMouseKey_ = -1;
    }
    if (hwnd_) {
        if (timerId_) {
            KillTimer(hwnd_, timerId_);
            timerId_ = 0;
        }
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (memDc_) {
        if (oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (memBitmap_) {
            DeleteObject(memBitmap_);
            memBitmap_ = nullptr;
        }
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }
}

void PluginEditorWin32::idle() noexcept {
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK PluginEditorWin32::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PluginEditorWin32* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = static_cast<PluginEditorWin32*>(cs->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<PluginEditorWin32*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT PluginEditorWin32::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            if (wParam == kTimerId60Fps && hwnd_) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd_, &ps);
            if (memDc_) {
                paint(memDc_);
                BitBlt(hdc, 0, 0, kWidth, kHeight, memDc_, 0, 0, SRCCOPY);
            } else {
                paint(hdc);
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            SetCapture(hwnd_);
            onMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_RBUTTONDOWN:
            SetFocus(hwnd_);
            onMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDBLCLK: {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int knobIdx = hitTestKnob(x, y);
            if (knobIdx >= 0) {
                const int paramId = kDisplayedKnobs[knobIdx];
                processor_.setParameterAutomated(paramId, kDefaultKnobValues[paramId]);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE:
            if (wParam & MK_LBUTTON) {
                onMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            ReleaseCapture();
            onMouseUp();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &pt);
            const int knobIdx = hitTestKnob(pt.x, pt.y);
            if (knobIdx >= 0) {
                const int paramId = kDisplayedKnobs[knobIdx];
                const float delta = (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? 0.025f : -0.025f;
                const float cur = processor_.getEngine().getParameterNormalized(paramId);
                processor_.setParameterAutomated(paramId, std::clamp(cur + delta, 0.0f, 1.0f));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        default:
            return DefWindowProcA(hwnd_, msg, wParam, lParam);
    }
}

void PluginEditorWin32::paint(HDC hdc) {
    RECT fullRect{0, 0, kWidth, kHeight};
    fillRectColor(hdc, fullRect, kColBgMain);

    SetBkMode(hdc, TRANSPARENT);
    drawHeaderBar(hdc);
    drawOscilloscope(hdc);
    drawKnobs(hdc);
    drawKeyboard(hdc);
}

void PluginEditorWin32::drawHeaderBar(HDC hdc) {
    RECT headerRect{0, 0, kWidth, 52};
    fillRectColor(hdc, headerRect, kColBgHeader);

    HPEN borderPen = CreatePen(PS_SOLID, 1, kColBorder);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
    MoveToEx(hdc, 0, 51, nullptr);
    LineTo(hdc, kWidth, 51);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    HFONT titleFont = CreateFontA(19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT subFont = CreateFontA(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, titleFont));
    SetTextColor(hdc, kColCyanWave);
    RECT titleR{16, 7, 234, 29};
    DrawTextA(hdc, "VOCODER THIN", -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, subFont);
    SetTextColor(hdc, kColTextDim);
    RECT subR{16, 27, 234, 46};
    DrawTextA(hdc, "MIDI VOCAL HARMONIZER & VOCODER", -1, &subR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    auto drawHeaderButton = [&](const RECT& r, const char* text, bool highlighted) {
        fillRectColor(hdc, r, highlighted ? kColBtnActive : kColBtnBg);
        frameRectColor(hdc, r, highlighted ? kColCyanWave : kColBorder);
        SetTextColor(hdc, kColTextBright);
        RECT tr = r;
        tr.left += 4;
        tr.right -= 4;
        DrawTextA(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    };

    const std::string srcText = processor_.getInputSourceBannerText();
    drawHeaderButton(kRectBtnSource, srcText.c_str(),
                     processor_.getEngine().getInputSourceMode() == InputSourceMode::kTestVocalGenerator);

    std::string micText = "MIC: " + processor_.getMicDeviceBannerText();
    drawHeaderButton(kRectBtnMicDev, micText.c_str(), processor_.getMicDevice().isRunning());

    const bool isLock = processor_.getEngine().getParameterNormalized(kParamPitchMode) < 0.5f;
    drawHeaderButton(kRectBtnMode, isLock ? "MODE: MIDI LOCK" : "MODE: RELATIVE", isLock);

    std::string presetText = std::string("PRE: ") + processor_.getProgramName(processor_.getProgram());
    drawHeaderButton(kRectBtnPreset, presetText.c_str(), false);

    SelectObject(hdc, oldFont);
    DeleteObject(titleFont);
    DeleteObject(subFont);
}

void PluginEditorWin32::drawOscilloscope(HDC hdc) {
    const RECT scopeRect{14, 60, kWidth - 14, 232};
    fillRectColor(hdc, scopeRect, RGB(12, 15, 22));
    frameRectColor(hdc, scopeRect, kColBorder);

    const int plotLeft = scopeRect.left + 10;
    const int plotRight = scopeRect.right - 56;
    const int plotTop = scopeRect.top + 30;
    const int plotBottom = scopeRect.bottom - 10;
    const int plotW = plotRight - plotLeft;
    const int plotH = plotBottom - plotTop;
    const int centerY = (plotTop + plotBottom) / 2;

    // Draw subtle oscilloscope grid lines
    HPEN gridPen = CreatePen(PS_SOLID, 1, kColGrid);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, gridPen));
    MoveToEx(hdc, plotLeft, centerY, nullptr);
    LineTo(hdc, plotRight, centerY);
    MoveToEx(hdc, plotLeft, plotTop + plotH / 4, nullptr);
    LineTo(hdc, plotRight, plotTop + plotH / 4);
    MoveToEx(hdc, plotLeft, plotTop + (plotH * 3) / 4, nullptr);
    LineTo(hdc, plotRight, plotTop + (plotH * 3) / 4);

    for (int gx = 1; gx < 8; ++gx) {
        const int x = plotLeft + (plotW * gx) / 8;
        MoveToEx(hdc, x, plotTop, nullptr);
        LineTo(hdc, x, plotBottom);
    }

    // Fetch latest 1024 samples from lock-free ScopeBuffer
    constexpr std::size_t kFetchCount = 1024;
    constexpr std::size_t kDrawSamples = 512;
    float inWave[kFetchCount]{};
    float outWave[kFetchCount]{};
    processor_.getEngine().getScopeBuffer().copyLatest(inWave, outWave, kFetchCount);

    // Find rising zero-crossing trigger in first 400 samples for steady wavelength display
    std::size_t trigOffset = 0;
    for (std::size_t i = 8; i < 400; ++i) {
        if (inWave[i - 1] <= 0.0f && inWave[i] > 0.0f && (inWave[i + 4] - inWave[i - 4]) > 0.01f) {
            trigOffset = i;
            break;
        }
    }

    // 1. Draw Harmonized Output Wavelength (Magenta/Coral)
    HPEN outPen = CreatePen(PS_SOLID, 2, kColMagentaWave);
    SelectObject(hdc, outPen);
    for (std::size_t i = 0; i < kDrawSamples; ++i) {
        const int x = plotLeft + static_cast<int>((i * static_cast<std::size_t>(plotW)) / (kDrawSamples - 1));
        const float s = std::clamp(outWave[trigOffset + i], -1.0f, 1.0f);
        const int y = centerY - static_cast<int>(s * static_cast<float>(plotH * 0.44f));
        if (i == 0) {
            MoveToEx(hdc, x, y, nullptr);
        } else {
            LineTo(hdc, x, y);
        }
    }

    // 2. Draw Live Microphone Input Wavelength (Electric Cyan)
    HPEN inPen = CreatePen(PS_SOLID, 2, kColCyanWave);
    SelectObject(hdc, inPen);
    for (std::size_t i = 0; i < kDrawSamples; ++i) {
        const int x = plotLeft + static_cast<int>((i * static_cast<std::size_t>(plotW)) / (kDrawSamples - 1));
        const float s = std::clamp(inWave[trigOffset + i], -1.0f, 1.0f);
        const int y = centerY - static_cast<int>(s * static_cast<float>(plotH * 0.44f));
        if (i == 0) {
            MoveToEx(hdc, x, y, nullptr);
        } else {
            LineTo(hdc, x, y);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);
    DeleteObject(outPen);
    DeleteObject(inPen);

    // Draw Input / Output level meters on the right side of the oscilloscope
    const float inRms = std::clamp(processor_.getEngine().getInputLevelRms() * 2.5f, 0.0f, 1.0f);
    const float outRms = std::clamp(processor_.getEngine().getOutputLevelRms() * 2.0f, 0.0f, 1.0f);

    RECT meterInBg{plotRight + 10, plotTop, plotRight + 24, plotBottom};
    RECT meterOutBg{plotRight + 30, plotTop, plotRight + 44, plotBottom};
    fillRectColor(hdc, meterInBg, RGB(24, 28, 40));
    fillRectColor(hdc, meterOutBg, RGB(24, 28, 40));

    RECT meterInFill = meterInBg;
    meterInFill.top = meterInBg.bottom - static_cast<int>(inRms * static_cast<float>(plotH));
    fillRectColor(hdc, meterInFill, kColCyanWave);

    RECT meterOutFill = meterOutBg;
    meterOutFill.top = meterOutBg.bottom - static_cast<int>(outRms * static_cast<float>(plotH));
    fillRectColor(hdc, meterOutFill, kColMagentaWave);

    // Draw HUD telemetry header inside oscilloscope
    HFONT hudFont = CreateFontA(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Consolas");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hudFont));

    char pitchBuf[64]{};
    formatFrequencyAndNote(processor_.getEngine().getDetectedPitchHz(), pitchBuf, sizeof(pitchBuf));
    char leftHud[128]{};
    std::snprintf(leftHud, sizeof(leftHud), "MIC WAVELENGTH (CYAN)  |  VOICE PITCH: %s", pitchBuf);
    SetTextColor(hdc, kColCyanWave);
    RECT hudL{plotLeft, scopeRect.top + 6, plotLeft + 420, scopeRect.top + 26};
    DrawTextA(hdc, leftHud, -1, &hudL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Active MIDI chord notes readout
    static const char* kNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    std::string chordStr = "HARMONY (MAGENTA)  |  MIDI: ";
    int activeCount = 0;
    for (int n = 0; n < 128; ++n) {
        if (processor_.getEngine().isNoteActive(n)) {
            char nb[16]{};
            std::snprintf(nb, sizeof(nb), "%s%d ", kNames[n % 12], (n / 12) - 1);
            chordStr += nb;
            ++activeCount;
        }
    }
    if (activeCount == 0) {
        chordStr += "NONE (PLAY MIDI OR CLICK KEYS)";
    }
    SetTextColor(hdc, kColMagentaWave);
    RECT hudR{plotLeft + 380, scopeRect.top + 6, plotRight, scopeRect.top + 26};
    DrawTextA(hdc, chordStr.c_str(), -1, &hudR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    DeleteObject(hudFont);
}

int PluginEditorWin32::hitTestKnob(int x, int y) const noexcept {
    if (y < 245 || y > 385) return -1;
    const int panelLeft = 18;
    const int slotW = (kWidth - 36) / 9;
    for (int i = 0; i < 9; ++i) {
        const int cx = panelLeft + i * slotW + slotW / 2;
        const int cy = 315;
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= 34 * 34) {
            return i;
        }
    }
    return -1;
}

void PluginEditorWin32::drawKnobs(HDC hdc) {
    RECT knobPanel{14, 240, kWidth - 14, 388};
    fillRectColor(hdc, knobPanel, kColBgPanel);
    frameRectColor(hdc, knobPanel, kColBorder);

    HFONT labelFont = CreateFontA(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT valFont = CreateFontA(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Consolas");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, labelFont));

    const int panelLeft = 18;
    const int slotW = (kWidth - 36) / 9;
    constexpr int kRadius = 26;

    for (int i = 0; i < 9; ++i) {
        const int paramId = kDisplayedKnobs[i];
        const float norm = processor_.getEngine().getParameterNormalized(paramId);
        const int cx = panelLeft + i * slotW + slotW / 2;
        const int cy = 314;

        // Parameter title above knob
        SelectObject(hdc, labelFont);
        SetTextColor(hdc, kColTextBright);
        RECT nameR{panelLeft + i * slotW, 248, panelLeft + (i + 1) * slotW, 272};
        DrawTextA(hdc, HarmonizerEngine::getParameterName(paramId), -1, &nameR,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Outer ring + knob body
        HBRUSH bodyBrush = CreateSolidBrush(RGB(18, 22, 32));
        HPEN ringPen = CreatePen(PS_SOLID, 2, (draggingKnob_ == i) ? kColCyanWave : kColBorder);
        HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, bodyBrush));
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, ringPen));
        Ellipse(hdc, cx - kRadius, cy - kRadius, cx + kRadius, cy + kRadius);

        // Draw active value arc ticks around knob (-135 deg to +135 deg)
        constexpr float kStartAngle = -0.75f * kPi;
        constexpr float kSweepAngle = 1.50f * kPi;
        const int numTicks = 18;
        for (int t = 0; t <= numTicks; ++t) {
            const float tf = static_cast<float>(t) / static_cast<float>(numTicks);
            const float ang = kStartAngle + tf * kSweepAngle - (kPi * 0.5f);
            const int x1 = cx + static_cast<int>(std::cos(ang) * (kRadius + 4));
            const int y1 = cy + static_cast<int>(std::sin(ang) * (kRadius + 4));
            const bool lit = (tf <= norm + 0.01f);
            SetPixel(hdc, x1, y1, lit ? kColCyanWave : kColBorder);
            SetPixel(hdc, x1 + 1, y1, lit ? kColCyanWave : kColBorder);
        }

        // Indicator line from center to rim
        const float valAngle = kStartAngle + norm * kSweepAngle - (kPi * 0.5f);
        const int ix = cx + static_cast<int>(std::cos(valAngle) * (kRadius - 5));
        const int iy = cy + static_cast<int>(std::sin(valAngle) * (kRadius - 5));
        HPEN needlePen = CreatePen(PS_SOLID, 3, (i == 2 || i == 3) ? kColAccentAmber : kColCyanWave);
        SelectObject(hdc, needlePen);
        MoveToEx(hdc, cx, cy, nullptr);
        LineTo(hdc, ix, iy);

        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(bodyBrush);
        DeleteObject(ringPen);
        DeleteObject(needlePen);

        // Formatted value + unit below knob
        char disp[24]{};
        processor_.getEngine().getParameterDisplay(paramId, disp, sizeof(disp));
        char fullVal[36]{};
        std::snprintf(fullVal, sizeof(fullVal), "%s %s", disp, HarmonizerEngine::getParameterLabel(paramId));

        SelectObject(hdc, valFont);
        SetTextColor(hdc, kColCyanWave);
        RECT valR{panelLeft + i * slotW, 354, panelLeft + (i + 1) * slotW, 380};
        DrawTextA(hdc, fullVal, -1, &valR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(labelFont);
    DeleteObject(valFont);
}

int PluginEditorWin32::hitTestPianoKey(int x, int y) const noexcept {
    const int kbLeft = 20;
    const int kbTop = 420;
    const int kbBottom = 504;
    const int whiteW = (kWidth - 40) / 15; // 15 white keys from C3 (48) to C5 (72)
    if (x < kbLeft || x >= kbLeft + whiteW * 15 || y < kbTop || y > kbBottom) {
        return -1;
    }

    // Check black keys first (upper 60% of keyboard height)
    const int blackBottom = kbTop + static_cast<int>((kbBottom - kbTop) * 0.60f);
    if (y <= blackBottom) {
        int whiteIdx = 0;
        for (int note = 48; note <= 72; ++note) {
            if (!isBlackKey(note)) {
                ++whiteIdx;
            } else {
                const int bxCenter = kbLeft + whiteIdx * whiteW;
                const int bwHalf = whiteW / 3;
                if (x >= bxCenter - bwHalf && x <= bxCenter + bwHalf) {
                    return note;
                }
            }
        }
    }

    // Otherwise map to white key
    const int targetWhite = (x - kbLeft) / whiteW;
    int whiteIdx = 0;
    for (int note = 48; note <= 72; ++note) {
        if (!isBlackKey(note)) {
            if (whiteIdx == targetWhite) {
                return note;
            }
            ++whiteIdx;
        }
    }
    return -1;
}

void PluginEditorWin32::drawKeyboard(HDC hdc) {
    RECT kbPanel{14, 394, kWidth - 14, 510};
    fillRectColor(hdc, kbPanel, kColBgPanel);
    frameRectColor(hdc, kbPanel, kColBorder);

    HFONT kbFont = CreateFontA(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, kbFont));

    SetTextColor(hdc, kColTextDim);
    RECT helpR{22, 396, 690, 418};
    DrawTextA(hdc, "MIDI HARMONY KEYBOARD (C3 - C5)  |  LEFT-CLICK: PLAY NOTE  |  RIGHT-CLICK: LATCH / HOLD CHORD",
              -1, &helpR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Clear Chord button
    fillRectColor(hdc, kRectBtnPanic, kColBtnBg);
    frameRectColor(hdc, kRectBtnPanic, kColBorder);
    SetTextColor(hdc, kColTextBright);
    RECT panicTextR = kRectBtnPanic;
    DrawTextA(hdc, "CLEAR CHORD", -1, &panicTextR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int kbLeft = 20;
    const int kbTop = 420;
    const int kbBottom = 504;
    const int whiteW = (kWidth - 40) / 15;
    const int blackBottom = kbTop + static_cast<int>((kbBottom - kbTop) * 0.60f);

    // 1. Draw White Keys
    int whiteIdx = 0;
    for (int note = 48; note <= 72; ++note) {
        if (isBlackKey(note)) continue;
        const bool active = processor_.getEngine().isNoteActive(note);
        RECT wr{kbLeft + whiteIdx * whiteW, kbTop, kbLeft + (whiteIdx + 1) * whiteW - 1, kbBottom};
        fillRectColor(hdc, wr, active ? kColCyanWave : RGB(228, 232, 240));
        frameRectColor(hdc, wr, RGB(30, 34, 46));

        if (note % 12 == 0) {
            char cLabel[8]{};
            std::snprintf(cLabel, sizeof(cLabel), "C%d", (note / 12) - 1);
            SetTextColor(hdc, RGB(20, 24, 34));
            RECT lr{wr.left, wr.bottom - 18, wr.right, wr.bottom - 2};
            DrawTextA(hdc, cLabel, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        ++whiteIdx;
    }

    // 2. Draw Black Keys
    whiteIdx = 0;
    for (int note = 48; note <= 72; ++note) {
        if (!isBlackKey(note)) {
            ++whiteIdx;
        } else {
            const bool active = processor_.getEngine().isNoteActive(note);
            const int bxCenter = kbLeft + whiteIdx * whiteW;
            const int bwHalf = whiteW / 3;
            RECT br{bxCenter - bwHalf, kbTop, bxCenter + bwHalf, blackBottom};
            fillRectColor(hdc, br, active ? kColMagentaWave : RGB(24, 27, 36));
            frameRectColor(hdc, br, RGB(10, 12, 18));
        }
    }

    SelectObject(hdc, oldFont);
    DeleteObject(kbFont);
}

void PluginEditorWin32::onMouseDown(int x, int y, bool rightButton) {
    POINT pt{x, y};

    if (!rightButton) {
        if (PtInRect(&kRectBtnSource, pt)) {
            processor_.cycleInputSourceMode();
            return;
        }
        if (PtInRect(&kRectBtnMicDev, pt)) {
            processor_.selectNextMicDevice();
            return;
        }
        if (PtInRect(&kRectBtnMode, pt)) {
            const float cur = processor_.getEngine().getParameterNormalized(kParamPitchMode);
            processor_.setParameterAutomated(kParamPitchMode, (cur < 0.5f) ? 1.0f : 0.0f);
            return;
        }
        if (PtInRect(&kRectBtnPreset, pt)) {
            processor_.cycleNextPreset();
            return;
        }
        if (PtInRect(&kRectBtnPanic, pt)) {
            processor_.getEngine().allNotesOff();
            return;
        }

        const int knobIdx = hitTestKnob(x, y);
        if (knobIdx >= 0) {
            draggingKnob_ = knobIdx;
            dragStartY_ = y;
            dragStartVal_ = processor_.getEngine().getParameterNormalized(kDisplayedKnobs[knobIdx]);
            return;
        }
    }

    const int note = hitTestPianoKey(x, y);
    if (note >= 0) {
        if (rightButton) {
            // Right-click toggles/latches note on or off for hands-free chord singing
            if (processor_.getEngine().isNoteActive(note)) {
                processor_.getEngine().noteOff(note);
            } else {
                processor_.getEngine().noteOn(note, 0.90f);
            }
        } else {
            heldMouseKey_ = note;
            processor_.getEngine().noteOn(note, 0.90f);
        }
    }
}

void PluginEditorWin32::onMouseMove(int x, int y) {
    if (draggingKnob_ >= 0) {
        const int paramId = kDisplayedKnobs[draggingKnob_];
        const float delta = static_cast<float>(dragStartY_ - y) / 130.0f;
        processor_.setParameterAutomated(paramId, std::clamp(dragStartVal_ + delta, 0.0f, 1.0f));
        return;
    }

    if (heldMouseKey_ >= 0) {
        const int note = hitTestPianoKey(x, y);
        if (note >= 0 && note != heldMouseKey_) {
            processor_.getEngine().noteOff(heldMouseKey_);
            heldMouseKey_ = note;
            processor_.getEngine().noteOn(note, 0.90f);
        }
    }
}

void PluginEditorWin32::onMouseUp() {
    draggingKnob_ = -1;
    if (heldMouseKey_ >= 0) {
        processor_.getEngine().noteOff(heldMouseKey_);
        heldMouseKey_ = -1;
    }
}

} // namespace vthin

