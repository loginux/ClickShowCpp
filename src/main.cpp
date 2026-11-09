#include <windows.h>
#include <objidl.h>
#include <oaidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "resource.h"
#include "version.h"
#include "settings.h"
#include "counts.h"
#include "input.h"
#include "overlay.h"
#include "util.h"

namespace {

constexpr UINT_PTR kTimerSave = 2;
constexpr UINT_PTR kTimerCounts = 3;
constexpr UINT_PTR kTimerScreenshot = 4;
constexpr UINT_PTR kTimerCaptureCheck = 5;

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kUpdateMessage = WM_APP + 2;

constexpr wchar_t kAppVersion[] = CLICKSHOW_VERSION_WSTRING;
constexpr wchar_t kHomepageUrl[] = L"https://github.com/loginux/ClickShowCpp";
constexpr wchar_t kReleasePageUrl[] = L"https://github.com/loginux/ClickShowCpp/releases";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ClickShowCpp";

// Control IDs indexed by clickshow::ButtonIndex. Keeping these tables next to
// the button constants means adding a button only requires one edit per row.
constexpr UINT kColorButtonIds[clickshow::kButtonCount] = {
    IDC_BTN_COLOR_LEFT, IDC_BTN_COLOR_MIDDLE, IDC_BTN_COLOR_RIGHT,
    IDC_BTN_COLOR_X1, IDC_BTN_COLOR_X2,
};
constexpr UINT kButtonCheckboxIds[clickshow::kButtonCount] = {
    IDC_CHK_BTN_LEFT, IDC_CHK_BTN_MIDDLE, IDC_CHK_BTN_RIGHT,
    IDC_CHK_BTN_X1, IDC_CHK_BTN_X2,
};
constexpr UINT kCountTextIds[clickshow::kButtonCount] = {
    IDC_TXT_LEFT, IDC_TXT_MIDDLE, IDC_TXT_RIGHT, IDC_TXT_X1, IDC_TXT_X2,
};

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HMENU g_trayMenu = nullptr;
NOTIFYICONDATAW g_trayIcon{};
bool g_trayIconAdded = false;
bool g_trayIconOwned = false;
std::atomic<bool> g_exiting{false};
UINT g_taskbarCreatedMessage = 0;
ULONG_PTR g_gdiplusToken = 0;
HWINEVENTHOOK g_foregroundHook = nullptr;

clickshow::Settings g_settings;
clickshow::SettingsStore g_settingsStore;
clickshow::CountStore g_counts;
clickshow::Overlay g_overlay;
clickshow::RawInput g_rawInput;

struct ButtonState {
    bool down = false;
    POINT point{};
    ULONGLONG tick = 0;
};

ButtonState g_buttonStates[clickshow::kButtonCount]{};

struct UpdateResult {
    std::wstring raw;
    std::wstring version;
    std::wstring url;
};

// The update check runs on a worker thread. Its result is handed to the UI
// thread through this mutex-protected slot instead of a raw pointer in the
// message LPARAM, so a stale/reused HWND can never receive a dangling pointer.
HANDLE g_updateCancelEvent = nullptr;
std::thread g_updateThread;
std::mutex g_updateMutex;
std::unique_ptr<UpdateResult> g_updateResult;

void SaveSettingsOnly();
void SaveCountsOnly();
void UpdateCountsUi();
void ScheduleSave();
void SetControlsFromSettings();
void ShowMainWindow();
void ExitApplication();
void OnInputButton(clickshow::InputButton button, bool down, POINT point);
void OnInputMove(POINT point);
void OnPrintScreen();
void OnCaptureShortcut();
bool IsAutoStartEnabled();
bool SetAutoStart(bool enable);
void UpdateCaptureSuppression();
void AddTrayIcon();
void RemoveTrayIcon();
void StartUpdateCheck();
void StopUpdateCheck();
std::unique_ptr<UpdateResult> FetchUpdate();
int ColorButtonIndex(UINT controlId);
int CheckboxButtonIndex(UINT controlId);
bool IsLightColor(COLORREF color);
void DrawColorButton(const DRAWITEMSTRUCT* item);
void ChooseColorForButton(UINT controlId);

void SaveSettingsOnly() {
    if (!g_settingsStore.Save(g_settings)) {
        clickshow::LogError(L"SaveSettings", L"写入设置失败");
    }
}

void SaveCountsOnly() {
    if (!g_counts.Save()) {
        clickshow::LogError(L"SaveCounts", L"写入计数失败");
    }
}

void UpdateCountsUi() {
    // The main window is hidden in the tray most of the time. Rewriting six
    // static controls on every single click is wasted work; ShowMainWindow()
    // refreshes the values whenever the window becomes visible again.
    if (g_mainWindow == nullptr || !IsWindowVisible(g_mainWindow)) {
        return;
    }
    for (int index = 0; index < clickshow::kButtonCount; ++index) {
        SetDlgItemTextW(g_mainWindow, kCountTextIds[index],
                        std::to_wstring(g_counts.Get(index)).c_str());
    }
    SetDlgItemTextW(g_mainWindow, IDC_TXT_TOTAL, std::to_wstring(g_counts.Total()).c_str());
}

void ScheduleSave() {
    if (g_mainWindow != nullptr) {
        // Re-arming the same timer id resets the 100 ms window, so a slider
        // drag or rapid toggling produces a single write after it settles.
        SetTimer(g_mainWindow, kTimerSave, 100, nullptr);
    }
}

void SetControlsFromSettings() {
    if (g_mainWindow == nullptr) {
        return;
    }

    auto setCheck = [&](int controlId, bool value) {
        CheckDlgButton(g_mainWindow, controlId, value ? BST_CHECKED : BST_UNCHECKED);
    };
    setCheck(IDC_CHK_CIRCLE, g_settings.enableClickCircle);
    setCheck(IDC_CHK_HOVER, g_settings.enableHoverDot);
    setCheck(IDC_CHK_COUNT, g_settings.enableClickCount);
    setCheck(IDC_CHK_HIDE_ON_CAPTURE, g_settings.hideDotDuringCapture);
    setCheck(IDC_CHK_AUTOSTART, IsAutoStartEnabled());

    auto setSlider = [&](int controlId, int value, int minimum, int maximum) {
        SendDlgItemMessageW(g_mainWindow, controlId, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
        SendDlgItemMessageW(g_mainWindow, controlId, TBM_SETTICFREQ, 10, 0);
        SendDlgItemMessageW(g_mainWindow, controlId, TBM_SETPOS, TRUE, value);
    };
    setSlider(IDC_SLIDER_SIZE, g_settings.indicatorSize, 50, 400);
    setSlider(IDC_SLIDER_HOVER, g_settings.hoverDotSize, 10, 150);

    SetDlgItemTextW(g_mainWindow, IDC_TXT_SIZE, std::to_wstring(g_settings.indicatorSize).c_str());
    SetDlgItemTextW(g_mainWindow, IDC_TXT_HOVER, std::to_wstring(g_settings.hoverDotSize).c_str());

    for (int index = 0; index < clickshow::kButtonCount; ++index) {
        setCheck(kButtonCheckboxIds[index], g_settings.buttons[index].enabled);
        InvalidateRect(GetDlgItem(g_mainWindow, kColorButtonIds[index]), nullptr, TRUE);
    }
    InvalidateRect(GetDlgItem(g_mainWindow, IDC_BTN_COLOR_HOVER), nullptr, TRUE);

    UpdateCountsUi();
}

std::wstring NormalizeRunValue(std::wstring value) {
    // Strip surrounding whitespace and the quotes we add around the path.
    const size_t begin = value.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) {
        return {};
    }
    const size_t end = value.find_last_not_of(L" \t");
    value = value.substr(begin, end - begin + 1);
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool IsAutoStartEnabled() {
    const std::wstring executable = clickshow::GetExePath();
    if (executable.empty()) {
        return false;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                            reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size < sizeof(wchar_t) || size > sizeof(buffer)) {
        return false;
    }

    // A stale value from an old install (or another app) must not show as
    // "enabled": compare the actual command with this executable.
    const std::wstring value = NormalizeRunValue(
        std::wstring(buffer, size / sizeof(wchar_t)));
    return !value.empty() &&
           CompareStringOrdinal(value.c_str(), -1, executable.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool SetAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    bool ok = true;
    if (enable) {
        const std::wstring executable = clickshow::GetExePath();
        if (executable.empty()) {
            ok = false;
        } else {
            // Quote the path: the Run key splits unquoted command lines on
            // spaces, which breaks installs under "Program Files" and allows
            // a stray C:\Program.exe to be picked up instead.
            const std::wstring command = L"\"" + executable + L"\"";
            ok = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }
    } else {
        const LSTATUS status = RegDeleteValueW(key, kRunValue);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    RegCloseKey(key);
    return ok;
}

void ShowMainWindow() {
    if (g_mainWindow == nullptr) {
        return;
    }
    if (IsIconic(g_mainWindow)) {
        ShowWindow(g_mainWindow, SW_RESTORE);
    } else {
        ShowWindow(g_mainWindow, SW_SHOW);
    }
    SetForegroundWindow(g_mainWindow);
    UpdateCountsUi();
}

void ExitApplication() {
    if (g_exiting) {
        return;
    }
    g_exiting = true;
    if (g_mainWindow != nullptr) {
        DestroyWindow(g_mainWindow);
    } else {
        PostQuitMessage(0);
    }
}

void OnInputButton(clickshow::InputButton button, bool down, POINT point) {
    if (g_exiting) {
        return;
    }

    const int index = static_cast<int>(button);
    if (index < 0 || index >= clickshow::kButtonCount) {
        return;
    }

    if (down) {
        g_buttonStates[index] = ButtonState{true, point, GetTickCount64()};

        if (g_settings.enableClickCount) {
            g_counts.Add(index);
            UpdateCountsUi();
        }

        if (g_settings.enableClickCircle && g_settings.buttons[index].enabled && !g_overlay.IsHidden()) {
            g_overlay.ShowRipple(index, true, point);
        }
    } else {
        ButtonState& state = g_buttonStates[index];
        if (!state.down) {
            return;
        }
        state.down = false;

        const long long dx = static_cast<long long>(point.x) - state.point.x;
        const long long dy = static_cast<long long>(point.y) - state.point.y;
        // Screen coordinates are physical pixels; scale the 200 logical px
        // threshold so the gesture feels the same on 150%/200% displays.
        const long long threshold = 200LL *
            static_cast<long long>(clickshow::GetDpiForPoint(point)) / 96LL;
        const bool movedFar = dx * dx + dy * dy > threshold * threshold;
        const bool longPress = GetTickCount64() - state.tick > 500;

        if (g_settings.enableClickCircle && g_settings.buttons[index].enabled &&
            !g_overlay.IsHidden() && (movedFar || longPress)) {
            g_overlay.ShowRipple(index, false, point);
        }
    }
}

void OnInputMove(POINT point) {
    if (g_exiting) {
        return;
    }
    g_overlay.UpdateHover(point);
}

void OnPrintScreen() {
    if (g_exiting) {
        return;
    }
    g_overlay.HideForScreenshot();
    if (g_mainWindow != nullptr) {
        // One shot: no need to poll every 50 ms for a full second.
        SetTimer(g_mainWindow, kTimerScreenshot, 1000, nullptr);
    }
}

void OnCaptureShortcut() {
    if (g_exiting || !g_settings.hideDotDuringCapture) {
        return;
    }
    // Hide right now: the tool is about to freeze the screen, and its own
    // overlay window (handled by UpdateCaptureSuppression) only shows up after
    // that. 400 ms is enough to cover the grab without a long visible gap when
    // a hotkey is used for something else.
    g_overlay.HideForScreenshot();
    if (g_mainWindow != nullptr) {
        SetTimer(g_mainWindow, kTimerScreenshot, 400, nullptr);
    }
}

// Screenshot tools (PixPin, Snipaste, the Snipping Tool...) normally put a
// borderless window over the whole virtual screen while you pick a region.
// The hover dot is topmost, so it sits on top of that window and ends up
// annoying both in the way and on screen. Detect that state and hide the dot.
// The actual capture is still excluded through WDA_EXCLUDEFROMCAPTURE; this
// only covers the time the tool itself is displayed.
struct FullscreenProbe {
    RECT window{};
    bool matched = false;
};

BOOL CALLBACK FullscreenMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* probe = reinterpret_cast<FullscreenProbe*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }
    const RECT& screen = info.rcMonitor;
    constexpr int kTolerance = 4;
    const bool covered =
        probe->window.left <= screen.left + kTolerance &&
        probe->window.top <= screen.top + kTolerance &&
        probe->window.right >= screen.right - kTolerance &&
        probe->window.bottom >= screen.bottom - kTolerance;
    if (covered) {
        probe->matched = true;
        return FALSE;
    }
    return TRUE;
}

bool IsFullscreenOtherAppWindow(HWND window) {
    if (window == nullptr || window == g_mainWindow) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return false;
    }
    if (!IsWindowVisible(window) || IsIconic(window)) {
        return false;
    }

    // A maximized ordinary window also covers the monitor (its DWM frame even
    // extends past it), so require a borderless window. That is what screenshot
    // tools and full-screen apps create; normal apps keep a caption.
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if ((style & WS_CAPTION) != 0) {
        return false;
    }

    // The desktop, taskbar and the task switcher are full-screen as well.
    wchar_t className[64]{};
    if (GetClassNameW(window, className, ARRAYSIZE(className)) > 0) {
        static const wchar_t* const kShellClasses[] = {
            L"Progman",
            L"WorkerW",
            L"Shell_TrayWnd",
            L"Shell_SecondaryTrayWnd",
            L"NotifyIconOverflowWindow",
            L"MultitaskingViewFrame",
            L"XamlExplorerHostIslandWindow",
        };
        for (const wchar_t* shellClass : kShellClasses) {
            if (wcscmp(className, shellClass) == 0) {
                return false;
            }
        }
    }

    FullscreenProbe probe;
    if (!GetWindowRect(window, &probe.window)) {
        return false;
    }
    // Some tools cover the whole virtual screen, others only the monitor with
    // the cursor, so check every monitor instead of just the bounding box.
    EnumDisplayMonitors(nullptr, nullptr, FullscreenMonitorProc,
                        reinterpret_cast<LPARAM>(&probe));
    return probe.matched;
}

void UpdateCaptureSuppression() {
    const bool suppress = g_settings.hideDotDuringCapture &&
                          IsFullscreenOtherAppWindow(GetForegroundWindow());
    g_overlay.SetCaptureSuppressed(suppress);
}

void CALLBACK ForegroundChangedProc(HWINEVENTHOOK, DWORD event, HWND, LONG idObject,
                                    LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW) {
        UpdateCaptureSuppression();
    }
}

void AddTrayIcon() {
    if (g_mainWindow == nullptr || g_trayIconAdded) {
        return;
    }

    // TaskbarCreated re-adds the icon; release the previously loaded one
    // before resetting the NOTIFYICONDATA structure.
    if (g_trayIcon.hIcon != nullptr && g_trayIconOwned) {
        DestroyIcon(g_trayIcon.hIcon);
    }
    g_trayIconOwned = false;

    g_trayIcon = NOTIFYICONDATAW{};
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_mainWindow;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = kTrayMessage;

    HICON icon = static_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_CLICKSHOW), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (icon != nullptr) {
        g_trayIconOwned = true;
    } else {
        // IDI_APPLICATION is a shared system icon; do not DestroyIcon it.
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    g_trayIcon.hIcon = icon;
    wcscpy_s(g_trayIcon.szTip, ARRAYSIZE(g_trayIcon.szTip), L"ClickShow\n鼠标点击提示器\n点击打开");

    if (Shell_NotifyIconW(NIM_ADD, &g_trayIcon)) {
        g_trayIconAdded = true;
    } else {
        clickshow::LogError(L"Tray.Add", clickshow::Win32ErrorMessage(GetLastError()));
    }

    if (g_trayMenu == nullptr) {
        g_trayMenu = CreatePopupMenu();
        AppendMenuW(g_trayMenu, MF_STRING, IDM_OPEN, L"打开");
        AppendMenuW(g_trayMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_trayMenu, MF_STRING, IDM_EXIT, L"退出");
    }
}

void RemoveTrayIcon() {
    if (g_trayIconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        g_trayIconAdded = false;
    }
    if (g_trayIcon.hIcon != nullptr && g_trayIconOwned) {
        DestroyIcon(g_trayIcon.hIcon);
        g_trayIconOwned = false;
    }
    g_trayIcon.hIcon = nullptr;
    if (g_trayMenu != nullptr) {
        DestroyMenu(g_trayMenu);
        g_trayMenu = nullptr;
    }
}

void StartUpdateCheck() {
    const HWND window = g_mainWindow;
    g_updateCancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_updateThread = std::thread([window]() {
        // Wait on a cancellable event so shutdown can interrupt the 10 s delay
        // and join the thread instead of leaving a detached thread running
        // while the static CRT is being torn down.
        if (g_updateCancelEvent != nullptr &&
            WaitForSingleObject(g_updateCancelEvent, 10000) != WAIT_TIMEOUT) {
            return;
        }
        if (g_exiting.load() || !IsWindow(window)) {
            return;
        }

        auto result = FetchUpdate();
        if (!result || g_exiting.load() || !IsWindow(window)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            g_updateResult = std::move(result);
        }
        // The message carries no pointer; the UI thread picks up the result.
        PostMessageW(window, kUpdateMessage, 0, 0);
    });
}

void StopUpdateCheck() {
    if (g_updateCancelEvent != nullptr) {
        SetEvent(g_updateCancelEvent);
    }
    if (g_updateThread.joinable()) {
        g_updateThread.join();
    }
    if (g_updateCancelEvent != nullptr) {
        CloseHandle(g_updateCancelEvent);
        g_updateCancelEvent = nullptr;
    }
}

std::unique_ptr<UpdateResult> FetchUpdate() {
    HINTERNET session = WinHttpOpen(L"ClickShowCpp/" CLICKSHOW_VERSION_WSTRING,
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr || g_exiting.load()) {
        if (session != nullptr) {
            WinHttpCloseHandle(session);
        }
        return nullptr;
    }
    // Keep these short: StopUpdateCheck() joins this thread on exit, so a hung
    // server must not make quitting the app appear to freeze.
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);

    HINTERNET connection = WinHttpConnect(session, L"helperservice.getquicker.cn",
                                          INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        return nullptr;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/clickshow/version",
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return nullptr;
    }

    std::unique_ptr<UpdateResult> result;

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            std::string body;
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
                    break;
                }
                // Cap each allocation as well as the total: a buggy or hostile
                // server must not be able to claim a multi-gigabyte chunk.
                const DWORD wanted = std::min<DWORD>(available, 8192);
                std::string chunk(wanted, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), wanted, &read) || read == 0) {
                    break;
                }
                body.append(chunk.data(), read);
                if (body.size() > 8192) {
                    body.resize(8192);
                    break;
                }
            }

            std::wstring text = clickshow::Trim(clickshow::Utf8ToWide(body));
            if (!text.empty()) {
                std::wstring version = text;
                std::wstring url;
                const size_t separator = text.find(L'|');
                if (separator != std::wstring::npos) {
                    version = clickshow::Trim(text.substr(0, separator));
                    url = clickshow::Trim(text.substr(separator + 1));
                }
                if (clickshow::CompareVersions(version, kAppVersion) > 0) {
                    result = std::make_unique<UpdateResult>();
                    result->raw = text;
                    result->version = version;
                    result->url = url.empty() ? kReleasePageUrl : url;
                }
            }
        }
    } else {
        clickshow::LogError(L"Update.Http", clickshow::Win32ErrorMessage(GetLastError()));
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

int ButtonIndexFromControlId(UINT controlId, const UINT (&ids)[clickshow::kButtonCount]) {
    for (int index = 0; index < clickshow::kButtonCount; ++index) {
        if (ids[index] == controlId) {
            return index;
        }
    }
    return -1;
}

int ColorButtonIndex(UINT controlId) {
    return ButtonIndexFromControlId(controlId, kColorButtonIds);
}

int CheckboxButtonIndex(UINT controlId) {
    return ButtonIndexFromControlId(controlId, kButtonCheckboxIds);
}

bool IsLightColor(COLORREF color) {
    const int luminance = (GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114) / 1000;
    return luminance > 140;
}

void DrawColorButton(const DRAWITEMSTRUCT* item) {
    if (item == nullptr || item->CtlType != ODT_BUTTON) {
        return;
    }

    COLORREF color = RGB(0, 0, 0);
    if (item->CtlID == IDC_BTN_COLOR_HOVER) {
        color = g_settings.hoverDotColor;
    } else {
        const int index = ColorButtonIndex(item->CtlID);
        if (index < 0) {
            return;
        }
        color = g_settings.buttons[index].color;
    }

    HBRUSH brush = CreateSolidBrush(color);
    FillRect(item->hDC, &item->rcItem, brush);
    DeleteObject(brush);
    FrameRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, IsLightColor(color) ? RGB(0, 0, 0) : RGB(255, 255, 255));
    RECT textRect = item->rcItem;
    DrawTextW(item->hDC, L"颜色", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if ((item->itemState & ODS_FOCUS) != 0) {
        RECT focusRect = item->rcItem;
        InflateRect(&focusRect, -2, -2);
        DrawFocusRect(item->hDC, &focusRect);
    }
}

void ChooseColorForButton(UINT controlId) {
    const bool hover = controlId == IDC_BTN_COLOR_HOVER;
    const int index = ColorButtonIndex(controlId);
    if (!hover && index < 0) {
        return;
    }

    COLORREF initial = hover ? g_settings.hoverDotColor : g_settings.buttons[index].color;
    static COLORREF customColors[16]{};

    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.hwndOwner = g_mainWindow;
    choice.rgbResult = initial;
    choice.lpCustColors = customColors;
    choice.Flags = CC_FULLOPEN | CC_RGBINIT;

    if (!ChooseColorW(&choice)) {
        return;
    }

    if (hover) {
        g_settings.hoverDotColor = choice.rgbResult;
    } else {
        g_settings.buttons[index].color = choice.rgbResult;
    }

    InvalidateRect(GetDlgItem(g_mainWindow, controlId), nullptr, TRUE);
    ScheduleSave();
}

INT_PTR CALLBACK MainDlgProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
        g_trayIconAdded = false;
        AddTrayIcon();
        return TRUE;
    }

    switch (message) {
    case WM_INITDIALOG: {
        g_mainWindow = window;
        SetWindowTextW(window, L"ClickShow " CLICKSHOW_VERSION_WSTRING L" (C++)");

        HICON bigIcon = static_cast<HICON>(LoadImageW(
            g_instance, MAKEINTRESOURCEW(IDI_CLICKSHOW), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        HICON smallIcon = static_cast<HICON>(LoadImageW(
            g_instance, MAKEINTRESOURCEW(IDI_CLICKSHOW), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (bigIcon != nullptr) {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
        }
        if (smallIcon != nullptr) {
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        }

        SetControlsFromSettings();
        ShowWindow(GetDlgItem(window, IDC_LBL_NEWVERSION), SW_HIDE);

        RECT rect{};
        if (GetWindowRect(window, &rect)) {
            const int width = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(window, nullptr, (screenWidth - width) / 2, (screenHeight - height) / 2,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return TRUE;
    }

    case WM_COMMAND: {
        const UINT controlId = LOWORD(wParam);
        switch (controlId) {
        case IDC_CHK_CIRCLE:
            g_settings.enableClickCircle = IsDlgButtonChecked(window, IDC_CHK_CIRCLE) == BST_CHECKED;
            ScheduleSave();
            return TRUE;

        case IDC_CHK_HOVER:
            g_settings.enableHoverDot = IsDlgButtonChecked(window, IDC_CHK_HOVER) == BST_CHECKED;
            ScheduleSave();
            return TRUE;

        case IDC_CHK_COUNT:
            g_settings.enableClickCount = IsDlgButtonChecked(window, IDC_CHK_COUNT) == BST_CHECKED;
            ScheduleSave();
            return TRUE;

        case IDC_CHK_HIDE_ON_CAPTURE:
            g_settings.hideDotDuringCapture =
                IsDlgButtonChecked(window, IDC_CHK_HIDE_ON_CAPTURE) == BST_CHECKED;
            UpdateCaptureSuppression();
            ScheduleSave();
            return TRUE;

        case IDC_CHK_AUTOSTART: {
            const bool enable = IsDlgButtonChecked(window, IDC_CHK_AUTOSTART) == BST_CHECKED;
            if (!SetAutoStart(enable)) {
                MessageBoxW(window, L"无法保存开机自动启动设置。", L"ClickShow",
                            MB_OK | MB_ICONWARNING);
                CheckDlgButton(window, IDC_CHK_AUTOSTART, enable ? BST_UNCHECKED : BST_CHECKED);
            }
            return TRUE;
        }

        case IDC_BTN_RESET_COUNT:
            if (MessageBoxW(window, L"确定要重置所有点击计数吗？", L"ClickShow",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_counts.Reset();
                SaveCountsOnly();
                UpdateCountsUi();
            }
            return TRUE;

        case IDC_BTN_DEFAULT: {
            const clickshow::Settings defaults = clickshow::Settings::Defaults();
            g_settings.indicatorSize = defaults.indicatorSize;
            g_settings.hoverDotSize = defaults.hoverDotSize;
            g_settings.hoverDotColor = defaults.hoverDotColor;
            g_settings.hoverDotAlpha = defaults.hoverDotAlpha;
            for (int index = 0; index < clickshow::kButtonCount; ++index) {
                g_settings.buttons[index].enabled = defaults.buttons[index].enabled;
                g_settings.buttons[index].color = defaults.buttons[index].color;
                g_settings.buttons[index].alpha = defaults.buttons[index].alpha;
            }
            SetControlsFromSettings();
            ScheduleSave();
            return TRUE;
        }

        case IDC_BTN_HOME:
            ShellExecuteW(window, L"open", kHomepageUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;

        case IDC_BTN_EXIT:
            ExitApplication();
            return TRUE;

        case IDM_OPEN:
            ShowMainWindow();
            return TRUE;

        case IDM_EXIT:
            ExitApplication();
            return TRUE;

        case IDC_BTN_COLOR_LEFT:
        case IDC_BTN_COLOR_MIDDLE:
        case IDC_BTN_COLOR_RIGHT:
        case IDC_BTN_COLOR_X1:
        case IDC_BTN_COLOR_X2:
        case IDC_BTN_COLOR_HOVER:
            ChooseColorForButton(controlId);
            return TRUE;

        case IDC_CHK_BTN_LEFT:
        case IDC_CHK_BTN_MIDDLE:
        case IDC_CHK_BTN_RIGHT:
        case IDC_CHK_BTN_X1:
        case IDC_CHK_BTN_X2: {
            const int index = CheckboxButtonIndex(controlId);
            if (index >= 0) {
                g_settings.buttons[index].enabled = IsDlgButtonChecked(window, controlId) == BST_CHECKED;
                ScheduleSave();
            }
            return TRUE;
        }

        default:
            break;
        }
        break;
    }

    case WM_HSCROLL: {
        HWND slider = reinterpret_cast<HWND>(lParam);
        if (slider == nullptr) {
            break;
        }
        const int controlId = GetDlgCtrlID(slider);
        if (controlId == IDC_SLIDER_SIZE) {
            const int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
            g_settings.indicatorSize = value;
            SetDlgItemTextW(window, IDC_TXT_SIZE, std::to_wstring(value).c_str());
            ScheduleSave();
            return TRUE;
        }
        if (controlId == IDC_SLIDER_HOVER) {
            const int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
            g_settings.hoverDotSize = value;
            SetDlgItemTextW(window, IDC_TXT_HOVER, std::to_wstring(value).c_str());
            ScheduleSave();
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
        DrawColorButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_LBL_NEWVERSION) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(255, 69, 0));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;

    case WM_TIMER:
        switch (wParam) {
        case clickshow::kOverlayTimerId:
            g_overlay.Tick();
            return TRUE;
        case kTimerSave:
            SaveSettingsOnly();
            g_overlay.ApplySettings(g_settings);
            KillTimer(window, kTimerSave);
            return TRUE;
        case kTimerCounts:
            SaveCountsOnly();
            return TRUE;
        case kTimerScreenshot:
            g_overlay.RestoreAfterScreenshot();
            KillTimer(window, kTimerScreenshot);
            return TRUE;
        case kTimerCaptureCheck:
            // Safety net for the foreground hook: it misses elevated tools and
            // tools that reuse an already-visible window. One check per second.
            UpdateCaptureSuppression();
            return TRUE;
        default:
            break;
        }
        break;

    case kTrayMessage: {
        const UINT event = static_cast<UINT>(lParam);
        if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
            ShowMainWindow();
            return TRUE;
        }
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            if (g_trayMenu != nullptr) {
                POINT point{};
                GetCursorPos(&point);
                SetForegroundWindow(window);
                TrackPopupMenu(g_trayMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                               point.x, point.y, 0, window, nullptr);
                PostMessageW(window, WM_NULL, 0, 0);
            }
            return TRUE;
        }
        break;
    }

    case kUpdateMessage: {
        std::unique_ptr<UpdateResult> result;
        {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            result = std::move(g_updateResult);
        }
        if (!result) {
            return TRUE;
        }
        if (g_settings.lastNotifiedVersion == result->raw) {
            ShowWindow(GetDlgItem(window, IDC_LBL_NEWVERSION), SW_SHOW);
            return TRUE;
        }

        // Original app records the raw version response before asking, so a
        // "No" answer is not asked again.
        g_settings.lastNotifiedVersion = result->raw;
        SaveSettingsOnly();

        const std::wstring question =
            L"ClickShow有新版本（" + result->version + L"），是否立即打开网页？";
        if (MessageBoxW(window, question.c_str(), L"ClickShow",
                        MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) == IDYES) {
            const HINSTANCE opened = ShellExecuteW(window, L"open", result->url.c_str(),
                                                   nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) <= 32) {
                MessageBoxW(window, (L"无法打开网址：" + result->url).c_str(), L"ClickShow",
                            MB_OK | MB_ICONWARNING | MB_TOPMOST);
            }
        }
        return TRUE;
    }

    case WM_CLOSE:
        if (g_exiting) {
            DestroyWindow(window);
        } else {
            ShowWindow(window, SW_HIDE);
        }
        return TRUE;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(window, SW_HIDE);
            return TRUE;
        }
        break;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        clickshow::InvalidateDpiCache();
        g_overlay.ReapplyDisplayAffinity();
        UpdateCaptureSuppression();
        break;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        // Windows may terminate the process right after this message without
        // ever sending WM_DESTROY. Counts are only flushed every 10 minutes,
        // so persist everything we have in memory right now.
        if (wParam != FALSE) {
            SaveSettingsOnly();
            SaveCountsOnly();
        }
        return TRUE;

    case WM_DESTROY:
        KillTimer(window, kTimerSave);
        KillTimer(window, kTimerCounts);
        KillTimer(window, kTimerScreenshot);
        KillTimer(window, kTimerCaptureCheck);
        KillTimer(window, clickshow::kOverlayTimerId);

        // Match OnClosed: settings first, then stop input, then counts and tray.
        SaveSettingsOnly();
        g_rawInput.Stop();
        g_overlay.Stop();
        SaveCountsOnly();
        RemoveTrayIcon();
        // Stop touching this HWND from any later callback: Windows may recycle
        // the handle value once the message loop finishes.
        g_mainWindow = nullptr;
        PostQuitMessage(0);
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;

    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\ClickShowCpp.SingleInstance");
    if (instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"ClickShow 已在运行。\n请查看系统托盘。", L"ClickShow",
                    MB_OK | MB_ICONINFORMATION);
        CloseHandle(instanceMutex);
        return 0;
    }

    const bool comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"初始化 GDI+ 失败。", L"ClickShow", MB_OK | MB_ICONERROR);
        if (comInitialized) {
            CoUninitialize();
        }
        if (instanceMutex != nullptr) {
            CloseHandle(instanceMutex);
        }
        return 1;
    }

    g_settings = g_settingsStore.Load();
    if (g_settingsStore.ConsumeLoadError()) {
        MessageBoxW(nullptr, L"配置文件损坏了，设置已恢复为默认值。", L"ClickShow",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
        g_settingsStore.Save(g_settings);
    }
    g_counts.Load();

    g_mainWindow = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainDlgProc, 0);
    if (g_mainWindow == nullptr) {
        MessageBoxW(nullptr, L"创建主窗口失败。", L"ClickShow", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        if (comInitialized) {
            CoUninitialize();
        }
        if (instanceMutex != nullptr) {
            CloseHandle(instanceMutex);
        }
        return 1;
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    if (!g_overlay.Start(instance, g_mainWindow, g_settings)) {
        clickshow::LogError(L"Startup.Overlay", L"覆盖层初始化失败");
    }

    // Event-driven: no polling. The callback is delivered on this thread.
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                       nullptr, ForegroundChangedProc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT);
    UpdateCaptureSuppression();

    clickshow::InputCallbacks callbacks;
    callbacks.onButton = OnInputButton;
    callbacks.onMove = OnInputMove;
    callbacks.onPrintScreen = OnPrintScreen;
    callbacks.onCaptureShortcut = OnCaptureShortcut;
    if (!g_rawInput.Start(instance, std::move(callbacks))) {
        clickshow::LogError(L"Startup.RawInput", L"全局输入初始化失败");
        MessageBoxW(nullptr, L"初始化全局鼠标输入失败，点击特效可能无法工作。", L"ClickShow",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }

    SetTimer(g_mainWindow, kTimerCounts, 10 * 60 * 1000, nullptr);
    SetTimer(g_mainWindow, kTimerCaptureCheck, 1000, nullptr);
    StartUpdateCheck();

    MSG message{};
    int exitCode = 0;
    for (;;) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            exitCode = static_cast<int>(message.wParam);
            break;
        }
        if (result == -1) {
            // Message loop failed unexpectedly; WM_DESTROY may never run, so
            // do not lose the in-memory settings and counts.
            SaveSettingsOnly();
            SaveCountsOnly();
            break;
        }
        if (!IsDialogMessageW(g_mainWindow, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    StopUpdateCheck();
    if (g_foregroundHook != nullptr) {
        UnhookWinEvent(g_foregroundHook);
        g_foregroundHook = nullptr;
    }
    g_rawInput.Stop();
    g_overlay.Stop();
    RemoveTrayIcon();

    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    if (comInitialized) {
        CoUninitialize();
    }
    if (instanceMutex != nullptr) {
        CloseHandle(instanceMutex);
    }
    return exitCode;
}
