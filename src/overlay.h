#pragma once
#include <windows.h>
#include <objidl.h>
#include <oaidl.h>
#include <gdiplus.h>
#include <memory>
#include <vector>

#include "settings.h"

namespace clickshow {

constexpr UINT_PTR kOverlayTimerId = 0x101;

class RippleWindow {
public:
    RippleWindow() = default;
    RippleWindow(const RippleWindow&) = delete;
    RippleWindow& operator=(const RippleWindow&) = delete;
    ~RippleWindow();

    bool Create(HINSTANCE instance);
    void Destroy();
    void Start(COLORREF color, BYTE baseAlpha, bool down, POINT point, UINT dpi, int logicalSize);
    bool Tick(ULONGLONG now);
    void Hide();
    void ReapplyDisplayAffinity();
    bool IsActive() const { return active_; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void EnsureSurface(int pixelSize);
    void Render(double progress);
    void UpdateLayered();

    HWND window_ = nullptr;
    HDC memoryDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> bitmap_;
    std::unique_ptr<Gdiplus::Graphics> graphics_;
    int pixelSize_ = 0;
    UINT dpi_ = 96;
    COLORREF color_ = RGB(0, 0, 0);
    BYTE baseAlpha_ = 0xFF;
    bool active_ = false;
    bool down_ = true;
    ULONGLONG startTick_ = 0;
    ULONGLONG durationMs_ = 400;
    POINT origin_{};
    static ATOM classAtom_;
};

class HoverWindow {
public:
    HoverWindow() = default;
    HoverWindow(const HoverWindow&) = delete;
    HoverWindow& operator=(const HoverWindow&) = delete;
    ~HoverWindow();

    bool Create(HINSTANCE instance);
    void Destroy();
    void Update(POINT point, UINT dpi, int logicalSize, COLORREF color, BYTE alpha);
    void Hide();
    void ReapplyDisplayAffinity();
    bool IsVisible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void EnsureSurface(int pixelSize);
    void Render();
    void UpdateLayered();

    HWND window_ = nullptr;
    HDC memoryDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> bitmap_;
    std::unique_ptr<Gdiplus::Graphics> graphics_;
    int pixelSize_ = 0;
    COLORREF color_ = RGB(255, 255, 90);
    BYTE alpha_ = 0x60;
    POINT origin_{};
    bool visible_ = false;
    bool surfaceDirty_ = true;
    static ATOM classAtom_;
};

class Overlay {
public:
    bool Start(HINSTANCE instance, HWND notifyWindow, const Settings& settings);
    void Stop();
    void ApplySettings(const Settings& settings);
    void ShowRipple(int button, bool down, POINT point);
    void UpdateHover(POINT point);
    void HideForScreenshot();
    void RestoreAfterScreenshot();
    // Used by the "hide the dot while another app covers the screen" option.
    void SetCaptureSuppressed(bool suppressed);
    void ReapplyDisplayAffinity();
    void Tick();
    bool IsHidden() const { return hidden_ || captureSuppressed_; }

private:
    RippleWindow* AcquireRipple();
    void EnsureTimer();
    void StopTimerIfIdle();
    void UpdateHoverNow();

    HINSTANCE instance_ = nullptr;
    HWND notifyWindow_ = nullptr;
    Settings settings_;
    std::vector<std::unique_ptr<RippleWindow>> ripples_;
    HoverWindow hover_;
    POINT hoverPoint_{};
    bool hoverDirty_ = false;
    bool hidden_ = false;
    bool captureSuppressed_ = false;
    bool timerRunning_ = false;
    size_t nextRipple_ = 0;
};

}
