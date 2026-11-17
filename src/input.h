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

// Numeric keypad keys need stable IDs even when Num Lock is off: Windows then
// reports VK_HOME/VK_END/VK_ARROW, which collide with the navigation cluster.
// These values sit in the reserved VK range and are only used as storage keys;
// they are never sent to Windows as virtual keys.
enum class PhysicalKey : BYTE {
    NumLock = 0xE0,
    NumpadDivide = 0xE1,
    NumpadMultiply = 0xE2,
    NumpadSubtract = 0xE3,
    Numpad7 = 0xE4,
    Numpad8 = 0xE5,
    Numpad9 = 0xE6,
    NumpadAdd = 0xE7,
    Numpad4 = 0xE8,
    Numpad5 = 0xE9,
    Numpad6 = 0xEA,
    Numpad1 = 0xEB,
    Numpad2 = 0xEC,
    Numpad3 = 0xED,
    NumpadEnter = 0xEE,
    Numpad0 = 0xEF,
    NumpadDecimal = 0xF0
};

struct InputCallbacks {
    std::function<void(InputButton, bool, POINT)> onButton;
    std::function<void(POINT)> onMove;
    std::function<void()> onPrintScreen;
    // Fired once on each key's down edge; auto-repeat is filtered out.
    std::function<void(BYTE)> onKey;
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

    // Feeds one raw mouse packet into the "is the mouse being taken over?"
    // detector. Returns true while an external program (KVM / remote control)
    // is driving the mouse, in which case the clicks belong to the remote
    // machine and must not be counted locally.
    bool TrackRemoteControl(POINT cursor, LONG dx, LONG dy, bool absolute);

    // True when the raw movement pushes the cursor further outside the
    // virtual desktop, i.e. the cursor is blocked by the screen edge rather
    // than held in place by another program.
    bool IsPushingOutwards(POINT cursor, LONG dx, LONG dy) const;

    HWND window_ = nullptr;
    POINT lastPosition_{};
    bool hasPosition_ = false;

    // GetMessageTime() of the most recent UP for each button. Mouse-gesture
    // tools can swallow a physical click and replay it as a complete synthetic
    // down/up pair just after the release; these timestamps let us recognise it.
    static constexpr int kButtonSlotCount = static_cast<int>(InputButton::X2) + 1;
    LONG lastUpTick_[kButtonSlotCount]{};
    bool hasLastUp_[kButtonSlotCount]{};

    // Remote-control detection state.
    POINT parkedCursor_{};
    LONG parkedTravelX_ = 0;
    LONG parkedTravelY_ = 0;
    ULONGLONG parkedSince_ = 0;
    bool hasParkedCursor_ = false;
    bool remoteControl_ = false;

    // Tracks which keys are physically down so WM_KEYDOWN auto-repeat (sent
    // continuously while a key is held) only produces a single count. Reset on
    // Stop() to avoid a stuck "down" flag if a WM_KEYUP is missed.
    bool keyDown_[256]{};

    InputCallbacks callbacks_;
    static ATOM classAtom_;
};

}
