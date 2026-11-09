#pragma once
#include <windows.h>
#include <functional>

namespace clickshow {

enum class InputButton {
    Left = 0,
    Middle = 1,
    Right = 2,
    X1 = 3,
    X2 = 4
};

struct InputCallbacks {
    std::function<void(InputButton, bool, POINT)> onButton;
    std::function<void(POINT)> onMove;
    std::function<void()> onPrintScreen;
};

class RawInput {
public:
    RawInput() = default;
    RawInput(const RawInput&) = delete;
    RawInput& operator=(const RawInput&) = delete;
    ~RawInput();

    bool Start(HINSTANCE instance, InputCallbacks callbacks);
    void Stop();
    HWND Window() const { return window_; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleInput(LPARAM lParam);

    HWND window_ = nullptr;
    POINT lastPosition_{};
    bool hasPosition_ = false;
    InputCallbacks callbacks_;
    static ATOM classAtom_;
};

}
