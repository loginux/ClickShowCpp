#include "input.h"
#include "util.h"

#include <utility>

namespace clickshow {

ATOM RawInput::classAtom_ = 0;

RawInput::~RawInput() {
    Stop();
}

bool RawInput::Start(HINSTANCE instance, InputCallbacks callbacks) {
    if (window_ != nullptr) {
        return true;
    }

    callbacks_ = std::move(callbacks);

    if (classAtom_ == 0) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WndProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"ClickShowRawInputWindow";
        classAtom_ = RegisterClassExW(&windowClass);
        if (classAtom_ == 0) {
            LogError(L"RawInput.RegisterClass", Win32ErrorMessage(GetLastError()));
            return false;
        }
    }

    window_ = CreateWindowExW(0, L"ClickShowRawInputWindow", L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, instance, this);
    if (window_ == nullptr) {
        LogError(L"RawInput.CreateWindow", Win32ErrorMessage(GetLastError()));
        return false;
    }

    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01;
    devices[0].usUsage = 0x02; // mouse
    devices[0].dwFlags = RIDEV_INPUTSINK;
    devices[0].hwndTarget = window_;

    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x06; // keyboard
    devices[1].dwFlags = RIDEV_INPUTSINK;
    devices[1].hwndTarget = window_;

    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        LogError(L"RawInput.RegisterDevices", Win32ErrorMessage(GetLastError()));
        DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    return true;
}

void RawInput::Stop() {
    if (window_ == nullptr) {
        return;
    }

    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01;
    devices[0].usUsage = 0x02;
    devices[0].dwFlags = RIDEV_REMOVE;
    devices[0].hwndTarget = nullptr;

    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x06;
    devices[1].dwFlags = RIDEV_REMOVE;
    devices[1].hwndTarget = nullptr;

    RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));

    DestroyWindow(window_);
    window_ = nullptr;
    // Forget the cached cursor position so a restart re-notifies the overlay.
    hasPosition_ = false;
    lastPosition_ = POINT{};
}

LRESULT CALLBACK RawInput::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = reinterpret_cast<RawInput*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    auto* self = reinterpret_cast<RawInput*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_INPUT && self != nullptr) {
        return self->HandleInput(lParam);
    }
    if (message == WM_DESTROY) {
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT RawInput::HandleInput(LPARAM lParam) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    // pcbSize is in/out: on success it holds the number of bytes copied (which
    // is smaller than sizeof(RAWINPUT) for keyboard packets), so comparing the
    // return value against it is meaningless. Check for the -1 error sentinel
    // and a plausible payload length instead.
    const UINT copied = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                                        &input, &size, sizeof(RAWINPUTHEADER));
    if (copied == static_cast<UINT>(-1) || size < sizeof(RAWINPUTHEADER) ||
        size > sizeof(input)) {
        return 0;
    }

    if (input.header.dwType == RIM_TYPEMOUSE) {
        POINT point{};
        if (!GetCursorPos(&point)) {
            return 0;
        }
        const RAWMOUSE& mouse = input.data.mouse;

        auto deliver = [&](USHORT downFlag, USHORT upFlag, InputButton button) {
            if ((mouse.usButtonFlags & downFlag) != 0 && callbacks_.onButton) {
                callbacks_.onButton(button, true, point);
            }
            if ((mouse.usButtonFlags & upFlag) != 0 && callbacks_.onButton) {
                callbacks_.onButton(button, false, point);
            }
        };

        deliver(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, InputButton::Left);
        deliver(RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, InputButton::Middle);
        deliver(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, InputButton::Right);
        deliver(RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, InputButton::X1);
        deliver(RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, InputButton::X2);

        // Raw Input sends a packet for every button/wheel event too. Only
        // notify the hover overlay when the cursor actually moved, otherwise a
        // high-polling-rate mouse causes needless redraws and timer wakeups.
        const bool positionChanged = !hasPosition_ ||
            point.x != lastPosition_.x || point.y != lastPosition_.y;
        if (positionChanged) {
            lastPosition_ = point;
            hasPosition_ = true;
            if (callbacks_.onMove) {
                callbacks_.onMove(point);
            }
        }
    } else if (input.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& keyboard = input.data.keyboard;
        const bool keyDown = keyboard.Message == WM_KEYDOWN || keyboard.Message == WM_SYSKEYDOWN;

        if (keyDown && keyboard.VKey == VK_SNAPSHOT) {
            if (callbacks_.onPrintScreen) {
                callbacks_.onPrintScreen();
            }
        } else if (keyDown && keyboard.VKey == 'S') {
            const bool winDown = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
            const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (winDown && shiftDown && callbacks_.onPrintScreen) {
                callbacks_.onPrintScreen();
            }
        }
    }

    return 0;
}

}
