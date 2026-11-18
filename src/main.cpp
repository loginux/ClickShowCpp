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
#include <cmath>
#include <cwchar>
#include <iterator>
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

clickshow::Settings g_settings;
clickshow::SettingsStore g_settingsStore;
clickshow::CountStore g_counts;
clickshow::KeyCountStore g_keyCounts;
clickshow::Overlay g_overlay;
HWND g_keyStatsWindow = nullptr;
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
void OnInputKey(BYTE vkey);
void SaveCountsOnly();
bool IsAutoStartEnabled();
bool SetAutoStart(bool enable);
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
    if (!g_keyCounts.Save()) {
        clickshow::LogError(L"SaveKeyCounts", L"写入键盘计数失败");
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
    setCheck(IDC_CHK_KEYCOUNT, g_settings.enableKeyboardCount);
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

void OnInputKey(BYTE vkey) {
    if (g_exiting || !g_settings.enableKeyboardCount) {
        return;
    }
    g_keyCounts.Add(vkey);
    // The dialog reads counts on each paint; a cheap invalidate keeps it live
    // while open without polling on a timer.
    if (g_keyStatsWindow != nullptr && IsWindowVisible(g_keyStatsWindow)) {
        InvalidateRect(g_keyStatsWindow, nullptr, FALSE);
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

// --- Keyboard heatmap ----------------------------------------------------

// One visible key: virtual key code plus width in "key units" (1.0 = a normal
// letter key). Layout follows a 104-key ANSI board; rows are laid out left to
// right in the order listed here.
struct HeatKey {
    BYTE vkey;
    int widthMilli; // width in 1/1000 key units, to express 1.25/1.75/2.25 etc.
    const wchar_t* label; // nullptr => derive from vkey
    int rowSpan = 1; // numpad +/- keys occupy two physical rows
};

struct HeatRow {
    const HeatKey* keys;
    int count;
};

constexpr HeatKey kRowNumber[] = {
    {VK_OEM_3, 1000, L"`"},  {'0', 1000, nullptr}, {'1', 1000, nullptr},
    {'2', 1000, nullptr},    {'3', 1000, nullptr}, {'4', 1000, nullptr},
    {'5', 1000, nullptr},    {'6', 1000, nullptr}, {'7', 1000, nullptr},
    {'8', 1000, nullptr},    {'9', 1000, nullptr}, {VK_OEM_MINUS, 1000, L"-"},
    {VK_OEM_PLUS, 1000, L"="}, {VK_BACK, 2000, L"Backspace"},
};
constexpr HeatKey kRowQ[] = {
    {VK_TAB, 1500, L"Tab"},  {'Q', 1000, nullptr},  {'W', 1000, nullptr},
    {'E', 1000, nullptr},    {'R', 1000, nullptr},  {'T', 1000, nullptr},
    {'Y', 1000, nullptr},    {'U', 1000, nullptr},  {'I', 1000, nullptr},
    {'O', 1000, nullptr},    {'P', 1000, nullptr},  {VK_OEM_4, 1000, L"["},
    {VK_OEM_6, 1000, L"]"},  {VK_OEM_5, 1500, L"\\"},
};
constexpr HeatKey kRowA[] = {
    {VK_CAPITAL, 1750, L"Caps"}, {'A', 1000, nullptr}, {'S', 1000, nullptr},
    {'D', 1000, nullptr},        {'F', 1000, nullptr}, {'G', 1000, nullptr},
    {'H', 1000, nullptr},        {'J', 1000, nullptr}, {'K', 1000, nullptr},
    {'L', 1000, nullptr},        {VK_OEM_1, 1000, L";"}, {VK_OEM_7, 1000, L"'"},
    {VK_RETURN, 2250, L"Enter"},
};
constexpr HeatKey kRowZ[] = {
    {VK_LSHIFT, 2250, L"Shift"}, {'Z', 1000, nullptr}, {'X', 1000, nullptr},
    {'C', 1000, nullptr},        {'V', 1000, nullptr}, {'B', 1000, nullptr},
    {'N', 1000, nullptr},        {'M', 1000, nullptr}, {VK_OEM_COMMA, 1000, L","},
    {VK_OEM_PERIOD, 1000, L"."}, {VK_OEM_2, 1000, L"/"}, {VK_RSHIFT, 2750, L"Shift"},
};
constexpr HeatKey kRowBottom[] = {
    {VK_LCONTROL, 1250, L"Ctrl"}, {VK_LWIN, 1250, L"Win"}, {VK_LMENU, 1250, L"Alt"},
    {VK_SPACE, 6250, L""},        {VK_RMENU, 1250, L"Alt"}, {VK_RWIN, 1250, L"Win"},
    {VK_APPS, 1250, L"Menu"},     {VK_RCONTROL, 1250, L"Ctrl"},
};
constexpr HeatKey kRowFunction[] = {
    {VK_ESCAPE, 1000, L"Esc"}, {VK_F1, 1000, L"F1"}, {VK_F2, 1000, L"F2"},
    {VK_F3, 1000, L"F3"},      {VK_F4, 1000, L"F4"}, {VK_F5, 1000, L"F5"},
    {VK_F6, 1000, L"F6"},      {VK_F7, 1000, L"F7"}, {VK_F8, 1000, L"F8"},
    {VK_F9, 1000, L"F9"},      {VK_F10, 1000, L"F10"}, {VK_F11, 1000, L"F11"},
    {VK_F12, 1000, L"F12"},
};
constexpr HeatKey kRowNav[] = {
    {VK_SNAPSHOT, 1000, L"PrtSc"}, {VK_SCROLL, 1000, L"ScrLk"}, {VK_PAUSE, 1000, L"Pause"},
    {VK_INSERT, 1000, L"Ins"},     {VK_HOME, 1000, L"Home"},    {VK_PRIOR, 1000, L"PgUp"},
    {VK_DELETE, 1000, L"Del"},     {VK_END, 1000, L"End"},      {VK_NEXT, 1000, L"PgDn"},
    {VK_UP, 1000, L"Up"},          {VK_LEFT, 1000, L"Left"},    {VK_DOWN, 1000, L"Down"},
    {VK_RIGHT, 1000, L"Right"},
};

constexpr HeatRow kHeatRows[] = {
    {kRowFunction, static_cast<int>(std::size(kRowFunction))},
    {kRowNumber,   static_cast<int>(std::size(kRowNumber))},
    {kRowQ,        static_cast<int>(std::size(kRowQ))},
    {kRowA,        static_cast<int>(std::size(kRowA))},
    {kRowZ,        static_cast<int>(std::size(kRowZ))},
    {kRowBottom,   static_cast<int>(std::size(kRowBottom))},
    {kRowNav,      static_cast<int>(std::size(kRowNav))},
};

constexpr BYTE Nk(clickshow::PhysicalKey key) {
    return static_cast<BYTE>(key);
}

// Numeric keypad cluster, aligned with rows 1..5 of the main block. The + and
// Enter keys span two rows, matching a full-size keyboard.
constexpr HeatKey kNumpadNumLock[] = {
    {Nk(clickshow::PhysicalKey::NumLock), 1000, L"Num"},
    {Nk(clickshow::PhysicalKey::NumpadDivide), 1000, L"/"},
    {Nk(clickshow::PhysicalKey::NumpadMultiply), 1000, L"*"},
    {Nk(clickshow::PhysicalKey::NumpadSubtract), 1000, L"-"},
};
constexpr HeatKey kNumpad7[] = {
    {Nk(clickshow::PhysicalKey::Numpad7), 1000, L"7"},
    {Nk(clickshow::PhysicalKey::Numpad8), 1000, L"8"},
    {Nk(clickshow::PhysicalKey::Numpad9), 1000, L"9"},
    {Nk(clickshow::PhysicalKey::NumpadAdd), 1000, L"+", 2},
};
constexpr HeatKey kNumpad4[] = {
    {Nk(clickshow::PhysicalKey::Numpad4), 1000, L"4"},
    {Nk(clickshow::PhysicalKey::Numpad5), 1000, L"5"},
    {Nk(clickshow::PhysicalKey::Numpad6), 1000, L"6"},
};
constexpr HeatKey kNumpad1[] = {
    {Nk(clickshow::PhysicalKey::Numpad1), 1000, L"1"},
    {Nk(clickshow::PhysicalKey::Numpad2), 1000, L"2"},
    {Nk(clickshow::PhysicalKey::Numpad3), 1000, L"3"},
    {Nk(clickshow::PhysicalKey::NumpadEnter), 1000, L"Ent", 2},
};
constexpr HeatKey kNumpad0[] = {
    {Nk(clickshow::PhysicalKey::Numpad0), 2000, L"0"},
    {Nk(clickshow::PhysicalKey::NumpadDecimal), 1000, L"."},
};

constexpr HeatRow kNumpadRows[] = {
    {kNumpadNumLock, static_cast<int>(std::size(kNumpadNumLock))},
    {kNumpad7,       static_cast<int>(std::size(kNumpad7))},
    {kNumpad4,       static_cast<int>(std::size(kNumpad4))},
    {kNumpad1,       static_cast<int>(std::size(kNumpad1))},
    {kNumpad0,       static_cast<int>(std::size(kNumpad0))},
};

std::wstring HeatKeyLabel(const HeatKey& key) {
    if (key.label != nullptr) {
        return key.label;
    }
    if (key.vkey >= '0' && key.vkey <= '9') {
        return std::wstring(1, static_cast<wchar_t>(key.vkey));
    }
    return std::wstring(1, static_cast<wchar_t>(key.vkey));
}

// Gamma-compressed blue: ratio^(1/gamma) keeps the long tail visible instead
// of letting the single hottest key saturate the whole scale.
COLORREF HeatColor(double ratio) {
    const double weight = std::pow(ratio, 0.5);
    const int base = 246;
    const int r = static_cast<int>(base + (30 - base) * weight);
    const int g = static_cast<int>(base + (120 - base) * weight);
    const int b = static_cast<int>(base + (220 - base) * weight);
    return RGB(r, g, b);
}

constexpr int kHeatKeyGap = 3;
constexpr int kHeatRowGap = 5;
constexpr int kHeatRowCount = static_cast<int>(std::size(kHeatRows));
constexpr double kHeatMainUnits = 16.0; // widest main row is 15 units + 1 gap
constexpr double kHeatTotalUnits = 20.5; // main block + gap + 4-column numpad
constexpr int kHeatNumpadFirstRow = 1;   // aligned with the number row

void TrackHeatMax(uint64_t& maxCount, const HeatRow& row) {
    for (int i = 0; i < row.count; ++i) {
        maxCount = (std::max)(maxCount, g_keyCounts.Get(row.keys[i].vkey));
    }
}

void DrawHeatKey(HDC dc, const HeatKey& key, int x, int y, int keyHeight,
                 double unitWidth, uint64_t maxCount) {
    const int width = static_cast<int>(unitWidth * key.widthMilli / 1000.0) - kHeatKeyGap;
    const int height = keyHeight * key.rowSpan + kHeatRowGap * (key.rowSpan - 1);
    const uint64_t count = g_keyCounts.Get(key.vkey);
    const double ratio = maxCount > 0 ? static_cast<double>(count) / maxCount : 0.0;
    RECT cell{x, y, x + width, y + height};

    HBRUSH brush = CreateSolidBrush(HeatColor(ratio));
    FillRect(dc, &cell, brush);
    DeleteObject(brush);
    FrameRect(dc, &cell, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    SetTextColor(dc, RGB(30, 30, 30));
    const std::wstring label = HeatKeyLabel(key);
    RECT labelRect = cell;
    labelRect.bottom = cell.top + height / 2;
    DrawTextW(dc, label.c_str(), -1, &labelRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    if (count > 0) {
        const std::wstring text = std::to_wstring(count);
        RECT countRect = cell;
        countRect.top = cell.top + height / 2;
        DrawTextW(dc, text.c_str(), -1, &countRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    }
}

void DrawKeyboardHeatmap(HDC dc, const RECT& area) {
    uint64_t maxCount = 0;
    for (const HeatRow& row : kHeatRows) {
        TrackHeatMax(maxCount, row);
    }
    for (const HeatRow& row : kNumpadRows) {
        TrackHeatMax(maxCount, row);
    }

    const int keyHeight =
        (area.bottom - area.top - (kHeatRowCount - 1) * kHeatRowGap) / kHeatRowCount;
    const double unitWidth =
        static_cast<double>(area.right - area.left) / kHeatTotalUnits;
    const int numpadX = area.left + static_cast<int>(kHeatMainUnits * unitWidth);

    HFONT font = CreateFontW(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);

    for (int r = 0; r < kHeatRowCount; ++r) {
        const HeatRow& row = kHeatRows[r];
        int x = area.left;
        const int y = area.top + r * (keyHeight + kHeatRowGap);
        for (int i = 0; i < row.count; ++i) {
            const HeatKey& key = row.keys[i];
            DrawHeatKey(dc, key, x, y, keyHeight, unitWidth, maxCount);
            x += static_cast<int>(unitWidth * key.widthMilli / 1000.0);
        }
    }

    for (int r = 0; r < static_cast<int>(std::size(kNumpadRows)); ++r) {
        const HeatRow& row = kNumpadRows[r];
        int x = numpadX;
        const int y = area.top +
            (kHeatNumpadFirstRow + r) * (keyHeight + kHeatRowGap);
        for (int i = 0; i < row.count; ++i) {
            const HeatKey& key = row.keys[i];
            DrawHeatKey(dc, key, x, y, keyHeight, unitWidth, maxCount);
            x += static_cast<int>(unitWidth * key.widthMilli / 1000.0);
        }
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);
}

INT_PTR CALLBACK KeyboardStatsDlgProc(HWND window, UINT message, WPARAM wParam, LPARAM) {
    switch (message) {
    case WM_INITDIALOG: {
        g_keyStatsWindow = window;
        SetDlgItemTextW(window, IDC_TXT_KEYTOTAL,
                        std::to_wstring(g_keyCounts.Total()).c_str());
        RECT rect{};
        if (GetWindowRect(window, &rect)) {
            const int w = rect.right - rect.left;
            const int h = rect.bottom - rect.top;
            const int sw = GetSystemMetrics(SM_CXSCREEN);
            const int sh = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(window, nullptr, (sw - w) / 2, (sh - h) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // Focus is assigned in ShowKeyboardStats() after the window is shown;
        // SetFocus() during WM_INITDIALOG has no effect on a hidden window.
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        // Fill the whole client area minus a top margin for the header row;
        // using the real client rect keeps the layout correct at any DPI.
        RECT area{client.left + 12, client.top + 48,
                  client.right - 12, client.bottom - 10};
        DrawKeyboardHeatmap(dc, area);
        SetDlgItemTextW(window, IDC_TXT_KEYTOTAL,
                        std::to_wstring(g_keyCounts.Total()).c_str());
        EndPaint(window, &ps);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_RESET_KEYCOUNT:
            if (MessageBoxW(window, L"确定要重置所有键盘计数吗？", L"ClickShow",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_keyCounts.Reset();
                SaveCountsOnly();
                SetDlgItemTextW(window, IDC_TXT_KEYTOTAL, L"0");
                InvalidateRect(window, nullptr, FALSE);
            }
            return TRUE;
        case IDCLOSE:
        case IDCANCEL:
            ShowWindow(window, SW_HIDE);
            return TRUE;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return TRUE;
    case WM_DESTROY:
        g_keyStatsWindow = nullptr;
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

void FocusStatsCloseButton(HWND window) {
    // A hidden window cannot own focus, so this must run after ShowWindow().
    // DM_SETDEFID also keeps Enter from activating the destructive reset button.
    SendMessageW(window, DM_SETDEFID, IDCLOSE, 0);
    SetFocus(GetDlgItem(window, IDCLOSE));
}

void ShowKeyboardStats() {
    if (g_keyStatsWindow != nullptr && IsWindow(g_keyStatsWindow)) {
        ShowWindow(g_keyStatsWindow, SW_SHOW);
        SetForegroundWindow(g_keyStatsWindow);
        FocusStatsCloseButton(g_keyStatsWindow);
        InvalidateRect(g_keyStatsWindow, nullptr, FALSE);
        return;
    }
    g_keyStatsWindow = CreateDialogParamW(g_instance, MAKEINTRESOURCEW(IDD_KEYBOARD_STATS),
                                          nullptr, KeyboardStatsDlgProc, 0);
    if (g_keyStatsWindow != nullptr) {
        ShowWindow(g_keyStatsWindow, SW_SHOW);
        FocusStatsCloseButton(g_keyStatsWindow);
    }
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

        case IDC_CHK_KEYCOUNT:
            g_settings.enableKeyboardCount = IsDlgButtonChecked(window, IDC_CHK_KEYCOUNT) == BST_CHECKED;
            ScheduleSave();
            return TRUE;

        case IDC_BTN_KEYSTATS:
            ShowKeyboardStats();
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
            g_settings.enableKeyboardCount = defaults.enableKeyboardCount;
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
        KillTimer(window, clickshow::kOverlayTimerId);

        // Match OnClosed: settings first, then stop input, then counts and tray.
        SaveSettingsOnly();
        if (g_keyStatsWindow != nullptr) {
            DestroyWindow(g_keyStatsWindow);
            g_keyStatsWindow = nullptr;
        }
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
    g_keyCounts.Load();

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

    clickshow::InputCallbacks callbacks;
    callbacks.onButton = OnInputButton;
    callbacks.onMove = OnInputMove;
    callbacks.onPrintScreen = OnPrintScreen;
    callbacks.onKey = OnInputKey;
    if (!g_rawInput.Start(instance, std::move(callbacks))) {
        clickshow::LogError(L"Startup.RawInput", L"全局输入初始化失败");
        MessageBoxW(nullptr, L"初始化全局鼠标输入失败，点击特效可能无法工作。", L"ClickShow",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }

    SetTimer(g_mainWindow, kTimerCounts, 10 * 60 * 1000, nullptr);
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
        const bool handledByStats = g_keyStatsWindow != nullptr &&
            IsWindowVisible(g_keyStatsWindow) && IsDialogMessageW(g_keyStatsWindow, &message);
        if (!handledByStats && !IsDialogMessageW(g_mainWindow, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    StopUpdateCheck();
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
