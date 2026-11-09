#include "overlay.h"
#include "util.h"

#include <algorithm>
#include <cmath>

using namespace Gdiplus;

namespace clickshow {
namespace {

constexpr wchar_t kRippleClassName[] = L"ClickShowRippleWindow";
constexpr wchar_t kHoverClassName[] = L"ClickShowHoverWindow";

// Both overlay windows are click-through, non-activating layered windows and
// have exactly the same message handling.
LRESULT CALLBACK LayeredWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED:
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

}

ATOM RippleWindow::classAtom_ = 0;
ATOM HoverWindow::classAtom_ = 0;

RippleWindow::~RippleWindow() {
    Destroy();
}

bool RippleWindow::Create(HINSTANCE instance) {
    if (classAtom_ == 0) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WndProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kRippleClassName;
        classAtom_ = RegisterClassExW(&windowClass);
        if (classAtom_ == 0) {
            LogError(L"Ripple.RegisterClass", Win32ErrorMessage(GetLastError()));
            return false;
        }
    }

    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kRippleClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        LogError(L"Ripple.CreateWindow", Win32ErrorMessage(GetLastError()));
        return false;
    }

    SetWindowDisplayAffinitySafe(window_);
    return true;
}

void RippleWindow::Destroy() {
    Hide();
    graphics_.reset();
    bitmap_.reset();
    if (memoryDc_ != nullptr) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
        dib_ = nullptr;
        bits_ = nullptr;
    }
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    pixelSize_ = 0;
}

void RippleWindow::EnsureSurface(int pixelSize) {
    if (pixelSize <= 0) {
        return;
    }
    if (pixelSize == pixelSize_ && bitmap_ != nullptr && graphics_ != nullptr && memoryDc_ != nullptr) {
        return;
    }

    graphics_.reset();
    bitmap_.reset();
    if (memoryDc_ != nullptr) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
        dib_ = nullptr;
        bits_ = nullptr;
    }

    pixelSize_ = pixelSize;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = pixelSize;
    info.bmiHeader.biHeight = -pixelSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib_ == nullptr || bits == nullptr) {
        LogError(L"Ripple.CreateDib", Win32ErrorMessage(GetLastError()));
        return;
    }
    bits_ = bits;

    memoryDc_ = CreateCompatibleDC(nullptr);
    if (memoryDc_ == nullptr) {
        LogError(L"Ripple.CreateDc", Win32ErrorMessage(GetLastError()));
        return;
    }
    SelectObject(memoryDc_, dib_);

    bitmap_ = std::make_unique<Bitmap>(pixelSize, pixelSize, pixelSize * 4,
                                       PixelFormat32bppPARGB, static_cast<BYTE*>(bits_));
    if (bitmap_->GetLastStatus() != Ok) {
        bitmap_.reset();
        return;
    }

    graphics_ = std::make_unique<Graphics>(bitmap_.get());
    if (graphics_->GetLastStatus() != Ok) {
        graphics_.reset();
        return;
    }
    graphics_->SetSmoothingMode(SmoothingModeAntiAlias);
    graphics_->SetPixelOffsetMode(PixelOffsetModeHalf);
}

void RippleWindow::Render(double progress) {
    if (graphics_ == nullptr || pixelSize_ <= 0) {
        return;
    }

    graphics_->Clear(Color(0, 0, 0, 0));

    const double factor = down_ ? (0.2 + 0.8 * progress) : (0.5 * progress);
    const float penWidth = std::max(1.0f, 3.0f * static_cast<float>(dpi_) / 96.0f);
    const double maxDiameter = std::max(2.0, static_cast<double>(pixelSize_) - penWidth - 1.0);
    const double diameter = std::clamp(maxDiameter * factor, 1.0, maxDiameter);

    // Match the WPF version: window opacity 0.95 on press, 0.7 on release,
    // multiplied by the button color alpha and the fading circle opacity.
    const double windowOpacity = down_ ? 0.95 : 0.7;
    int alphaValue = static_cast<int>(std::lround(
        255.0 * (1.0 - progress) * windowOpacity * (static_cast<double>(baseAlpha_) / 255.0)));
    alphaValue = std::clamp(alphaValue, 0, 255);

    const Color color(static_cast<BYTE>(alphaValue), GetRValue(color_), GetGValue(color_), GetBValue(color_));
    Pen pen(color, penWidth);
    const REAL offset = static_cast<REAL>((pixelSize_ - diameter) / 2.0);
    graphics_->DrawEllipse(&pen, offset, offset, static_cast<REAL>(diameter), static_cast<REAL>(diameter));
    graphics_->Flush(FlushIntentionSync);
    UpdateLayered();
}

void RippleWindow::UpdateLayered() {
    if (window_ == nullptr || memoryDc_ == nullptr || pixelSize_ <= 0) {
        return;
    }

    POINT destination{origin_.x, origin_.y};
    SIZE size{pixelSize_, pixelSize_};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, nullptr, &destination, &size, memoryDc_, &source, 0, &blend, ULW_ALPHA);
}

void RippleWindow::Start(COLORREF color, BYTE baseAlpha, bool down, POINT point, UINT dpi, int logicalSize) {
    color_ = color;
    baseAlpha_ = baseAlpha;
    down_ = down;
    dpi_ = dpi >= 48 ? dpi : 96;

    const int pixelSize = std::max(8, MulDiv(logicalSize, static_cast<int>(dpi_), 96));
    EnsureSurface(pixelSize);

    active_ = true;
    startTick_ = GetTickCount64();
    durationMs_ = down ? 400 : 300;
    origin_.x = point.x - pixelSize / 2;
    origin_.y = point.y - pixelSize / 2;

    SetWindowPos(window_, HWND_TOPMOST, origin_.x, origin_.y, pixelSize, pixelSize,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowDisplayAffinitySafe(window_);
    Render(0.0);
}

bool RippleWindow::Tick(ULONGLONG now) {
    if (!active_) {
        return false;
    }
    const double progress = durationMs_ > 0
        ? static_cast<double>(now - startTick_) / static_cast<double>(durationMs_)
        : 1.0;
    if (progress >= 1.0) {
        Hide();
        return false;
    }
    Render(progress);
    return true;
}

void RippleWindow::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
    active_ = false;
}

LRESULT CALLBACK RippleWindow::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return LayeredWndProc(window, message, wParam, lParam);
}

HoverWindow::~HoverWindow() {
    Destroy();
}

bool HoverWindow::Create(HINSTANCE instance) {
    if (classAtom_ == 0) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WndProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kHoverClassName;
        classAtom_ = RegisterClassExW(&windowClass);
        if (classAtom_ == 0) {
            LogError(L"Hover.RegisterClass", Win32ErrorMessage(GetLastError()));
            return false;
        }
    }

    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kHoverClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        LogError(L"Hover.CreateWindow", Win32ErrorMessage(GetLastError()));
        return false;
    }

    SetWindowDisplayAffinitySafe(window_);
    return true;
}

void HoverWindow::Destroy() {
    Hide();
    graphics_.reset();
    bitmap_.reset();
    if (memoryDc_ != nullptr) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
        dib_ = nullptr;
        bits_ = nullptr;
    }
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    pixelSize_ = 0;
}

void HoverWindow::EnsureSurface(int pixelSize) {
    if (pixelSize <= 0) {
        return;
    }
    if (pixelSize == pixelSize_ && !surfaceDirty_ && bitmap_ != nullptr &&
        graphics_ != nullptr && memoryDc_ != nullptr) {
        return;
    }

    // A freshly allocated DIB is empty; the circle has to be drawn again.
    surfaceDirty_ = true;
    graphics_.reset();
    bitmap_.reset();
    if (memoryDc_ != nullptr) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
        dib_ = nullptr;
        bits_ = nullptr;
    }

    pixelSize_ = pixelSize;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = pixelSize;
    info.bmiHeader.biHeight = -pixelSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib_ == nullptr || bits == nullptr) {
        LogError(L"Hover.CreateDib", Win32ErrorMessage(GetLastError()));
        return;
    }
    bits_ = bits;

    memoryDc_ = CreateCompatibleDC(nullptr);
    if (memoryDc_ == nullptr) {
        LogError(L"Hover.CreateDc", Win32ErrorMessage(GetLastError()));
        return;
    }
    SelectObject(memoryDc_, dib_);

    bitmap_ = std::make_unique<Bitmap>(pixelSize, pixelSize, pixelSize * 4,
                                       PixelFormat32bppPARGB, static_cast<BYTE*>(bits_));
    if (bitmap_->GetLastStatus() != Ok) {
        bitmap_.reset();
        return;
    }

    graphics_ = std::make_unique<Graphics>(bitmap_.get());
    if (graphics_->GetLastStatus() != Ok) {
        graphics_.reset();
        return;
    }
    graphics_->SetSmoothingMode(SmoothingModeAntiAlias);
    graphics_->SetPixelOffsetMode(PixelOffsetModeHalf);
}

void HoverWindow::Render() {
    if (graphics_ == nullptr || pixelSize_ <= 0) {
        return;
    }

    graphics_->Clear(Color(0, 0, 0, 0));
    SolidBrush brush(Color(alpha_, GetRValue(color_), GetGValue(color_), GetBValue(color_)));
    const REAL inset = std::max(0.5f, static_cast<REAL>(pixelSize_) * 0.03f);
    const REAL diameter = static_cast<REAL>(pixelSize_) - inset * 2.0f;
    graphics_->FillEllipse(&brush, inset, inset, diameter, diameter);
    graphics_->Flush(FlushIntentionSync);
    surfaceDirty_ = false;
    UpdateLayered();
}

void HoverWindow::UpdateLayered() {
    if (window_ == nullptr || memoryDc_ == nullptr || pixelSize_ <= 0) {
        return;
    }

    POINT destination{origin_.x, origin_.y};
    SIZE size{pixelSize_, pixelSize_};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, nullptr, &destination, &size, memoryDc_, &source, 0, &blend, ULW_ALPHA);
}

void HoverWindow::Update(POINT point, UINT dpi, int logicalSize, COLORREF color, BYTE alpha) {
    if (dpi < 48) {
        dpi = 96;
    }
    const int pixelSize = std::max(4, MulDiv(logicalSize, static_cast<int>(dpi), 96));
    const bool styleChanged = (color != color_) || (alpha != alpha_);
    color_ = color;
    alpha_ = alpha;
    EnsureSurface(pixelSize);

    origin_.x = point.x - pixelSize / 2;
    origin_.y = point.y - pixelSize / 2;

    // Move the window explicitly instead of relying on UpdateLayeredWindow's
    // pptDst to relocate it: the explicit call keeps the frame identical to the
    // previous behaviour, and the cost is tiny next to the GDI+ redraw we just
    // stopped doing. SWP_NOREDRAW is safe because UpdateLayeredWindow paints.
    const bool wasVisible = visible_;
    visible_ = true;
    UINT flags = SWP_NOACTIVATE | SWP_NOREDRAW;
    HWND insertAfter = nullptr;
    if (wasVisible) {
        flags |= SWP_NOZORDER;
    } else {
        insertAfter = HWND_TOPMOST;
        flags |= SWP_SHOWWINDOW;
    }
    SetWindowPos(window_, insertAfter, origin_.x, origin_.y, pixelSize, pixelSize, flags);
    if (!wasVisible) {
        SetWindowDisplayAffinitySafe(window_);
    }

    // The dot is a static image. Redraw it only when the surface, colour or
    // alpha changed; moving the cursor only needs the layered window update.
    if (surfaceDirty_ || styleChanged) {
        Render();
    } else {
        UpdateLayered();
    }
}

void HoverWindow::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
}

LRESULT CALLBACK HoverWindow::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return LayeredWndProc(window, message, wParam, lParam);
}

bool Overlay::Start(HINSTANCE instance, HWND notifyWindow, const Settings& settings) {
    instance_ = instance;
    notifyWindow_ = notifyWindow;
    settings_ = settings;

    const bool hoverOk = hover_.Create(instance);
    if (!hoverOk) {
        LogError(L"Overlay.Start", L"创建浮标窗口失败");
    }

    ApplySettings(settings);
    return hoverOk;
}

void Overlay::Stop() {
    if (timerRunning_ && notifyWindow_ != nullptr) {
        KillTimer(notifyWindow_, kOverlayTimerId);
        timerRunning_ = false;
    }

    for (auto& ripple : ripples_) {
        ripple->Destroy();
    }
    ripples_.clear();
    hover_.Destroy();

    hidden_ = false;
    hoverDirty_ = false;
    // Drop the window handle so a queued timer callback cannot call SetTimer on
    // a handle Windows may already have recycled.
    notifyWindow_ = nullptr;
}

void Overlay::ApplySettings(const Settings& settings) {
    settings_ = settings;

    if (hidden_ || !settings_.enableHoverDot) {
        hover_.Hide();
    } else {
        POINT point{};
        GetCursorPos(&point);
        hoverPoint_ = point;
        const UINT dpi = GetDpiForPoint(point);
        hover_.Update(point, dpi, settings_.hoverDotSize, settings_.hoverDotColor, settings_.hoverDotAlpha);
    }

    if (!settings_.enableClickCircle) {
        for (auto& ripple : ripples_) {
            ripple->Hide();
        }
        StopTimerIfIdle();
    }
}

void Overlay::ShowRipple(int button, bool down, POINT point) {
    if (hidden_ || !settings_.enableClickCircle) {
        return;
    }
    if (button < 0 || button >= kButtonCount || !settings_.buttons[button].enabled) {
        return;
    }

    RippleWindow* ripple = AcquireRipple();
    if (ripple == nullptr) {
        return;
    }

    const UINT dpi = GetDpiForPoint(point);
    ripple->Start(settings_.buttons[button].color, settings_.buttons[button].alpha,
                  down, point, dpi, settings_.indicatorSize);
    EnsureTimer();
}

void Overlay::UpdateHover(POINT point) {
    if (hidden_ || !settings_.enableHoverDot) {
        return;
    }
    hoverPoint_ = point;
    hoverDirty_ = true;
    EnsureTimer();
}

void Overlay::HideForScreenshot() {
    if (hidden_) {
        return;
    }
    hidden_ = true;
    for (auto& ripple : ripples_) {
        ripple->Hide();
    }
    hover_.Hide();
    hoverDirty_ = false;
    StopTimerIfIdle();
}

void Overlay::RestoreAfterScreenshot() {
    hidden_ = false;
    if (!settings_.enableHoverDot) {
        return;
    }
    POINT point{};
    GetCursorPos(&point);
    hoverPoint_ = point;
    hoverDirty_ = true;
    EnsureTimer();
}

void Overlay::Tick() {
    const ULONGLONG now = GetTickCount64();
    bool anyActive = false;

    for (auto& ripple : ripples_) {
        if (ripple->IsActive() && ripple->Tick(now)) {
            anyActive = true;
        }
    }

    if (hoverDirty_) {
        UpdateHoverNow();
        // Keep the timer alive while the pointer is moving. Killing it on the
        // first idle frame would force SetTimer/KillTimer churn on every raw
        // input packet; one continuous 16 ms timer is cheaper.
        anyActive = true;
    }

    if (!anyActive && !hoverDirty_) {
        StopTimerIfIdle();
    }
}

RippleWindow* Overlay::AcquireRipple() {
    for (auto& ripple : ripples_) {
        if (!ripple->IsActive()) {
            return ripple.get();
        }
    }

    if (ripples_.size() < 32) {
        auto ripple = std::make_unique<RippleWindow>();
        if (!ripple->Create(instance_)) {
            return nullptr;
        }
        ripples_.push_back(std::move(ripple));
        return ripples_.back().get();
    }

    RippleWindow* ripple = ripples_[nextRipple_ % ripples_.size()].get();
    ++nextRipple_;
    return ripple;
}

void Overlay::EnsureTimer() {
    if (timerRunning_ || notifyWindow_ == nullptr) {
        return;
    }
    SetTimer(notifyWindow_, kOverlayTimerId, 16, nullptr);
    timerRunning_ = true;
}

void Overlay::StopTimerIfIdle() {
    for (auto& ripple : ripples_) {
        if (ripple->IsActive()) {
            return;
        }
    }
    if (hoverDirty_) {
        return;
    }
    if (timerRunning_ && notifyWindow_ != nullptr) {
        KillTimer(notifyWindow_, kOverlayTimerId);
        timerRunning_ = false;
    }
}

void Overlay::UpdateHoverNow() {
    if (hidden_ || !settings_.enableHoverDot) {
        hover_.Hide();
        hoverDirty_ = false;
        return;
    }

    const UINT dpi = GetDpiForPoint(hoverPoint_);
    hover_.Update(hoverPoint_, dpi, settings_.hoverDotSize, settings_.hoverDotColor, settings_.hoverDotAlpha);
    hoverDirty_ = false;
}

}
