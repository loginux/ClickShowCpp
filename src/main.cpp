#include <windows.h>
#include <objidl.h>
#include <oaidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <objbase.h>

#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "resource.h"
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

constexpr wchar_t kAppVersion[] = L"1.5.0";
constexpr wchar_t kHomepageUrl[] = L"https://github.com/loginux/ClickShowCpp";
constexpr wchar_t kReleasePageUrl[] = L"https://github.com/loginux/ClickShowCpp/releases";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ClickShowCpp";

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HMENU g_trayMenu = nullptr;
NOTIFYICONDATAW g_trayIcon{};
bool g_trayIconAdded = false;
bool g_exiting = false;
UINT g_taskbarCreatedMessage = 0;
ULONGLONG g_screenshotHideUntil = 0;
bool g_screenshotHidden = false;
ULONG_PTR g_gdiplusToken = 0;

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
bool IsAutoStartEnabled();
bool SetAutoStart(bool enable);
void AddTrayIcon();
void RemoveTrayIcon();
void StartUpdateCheck();
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
    if (g_mainWindow == nullptr) {
        return;
    }
    auto setValue = [&](int controlId, uint64_t value) {
        SetDlgItemTextW(g_mainWindow, controlId, std::to_wstring(value).c_str());
    };
    setValue(IDC_TXT_LEFT, g_counts.Get(clickshow::kLeft));
    setValue(IDC_TXT_MIDDLE, g_counts.Get(clickshow::kMiddle));
    setValue(IDC_TXT_RIGHT, g_counts.Get(clickshow::kRight));
    setValue(IDC_TXT_X1, g_counts.Get(clickshow::kX1));
    setValue(IDC_TXT_X2, g_counts.Get(clickshow::kX2));
    setValue(IDC_TXT_TOTAL, g_counts.Total());
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

    setCheck(IDC_CHK_BTN_LEFT, g_settings.buttons[clickshow::kLeft].enabled);
    setCheck(IDC_CHK_BTN_MIDDLE, g_settings.buttons[clickshow::kMiddle].enabled);
    setCheck(IDC_CHK_BTN_RIGHT, g_settings.buttons[clickshow::kRight].enabled);
    setCheck(IDC_CHK_BTN_X1, g_settings.buttons[clickshow::kX1].enabled);
    setCheck(IDC_CHK_BTN_X2, g_settings.buttons[clickshow::kX2].enabled);

    const int colorButtons[] = {
        IDC_BTN_COLOR_LEFT, IDC_BTN_COLOR_MIDDLE, IDC_BTN_COLOR_RIGHT,
        IDC_BTN_COLOR_X1, IDC_BTN_COLOR_X2, IDC_BTN_COLOR_HOVER,
    };
    for (int controlId : colorButtons) {
        InvalidateRect(GetDlgItem(g_mainWindow, controlId), nullptr, TRUE);
    }

    UpdateCountsUi();
}

bool IsAutoStartEnabled() {
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
    return status == ERROR_SUCCESS && type == REG_SZ;
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
            ok = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(executable.c_str()),
                                static_cast<DWORD>((executable.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
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
        const bool movedFar = dx * dx + dy * dy > 200LL * 200LL;
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
    g_screenshotHidden = true;
    g_screenshotHideUntil = GetTickCount64() + 1000;
    g_overlay.HideForScreenshot();
    if (g_mainWindow != nullptr) {
        SetTimer(g_mainWindow, kTimerScreenshot, 50, nullptr);
    }
}

void AddTrayIcon() {
    if (g_mainWindow == nullptr || g_trayIconAdded) {
        return;
    }

    g_trayIcon = NOTIFYICONDATAW{};
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_mainWindow;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = kTrayMessage;

    HICON icon = static_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_CLICKSHOW), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (icon == nullptr) {
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
    if (g_trayMenu != nullptr) {
        DestroyMenu(g_trayMenu);
        g_trayMenu = nullptr;
    }
}

void StartUpdateCheck() {
    HWND window = g_mainWindow;
    std::thread([window]() {
        Sleep(10000);
        auto result = FetchUpdate();
        if (!result) {
            return;
        }
        auto* payload = new UpdateResult(std::move(*result));
        if (!PostMessageW(window, kUpdateMessage, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

std::unique_ptr<UpdateResult> FetchUpdate() {
    HINTERNET session = WinHttpOpen(L"ClickShowCpp/1.5", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return nullptr;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

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
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                    break;
                }
                body.append(chunk.data(), read);
                if (body.size() > 8192) {
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

int ColorButtonIndex(UINT controlId) {
    switch (controlId) {
    case IDC_BTN_COLOR_LEFT: return clickshow::kLeft;
    case IDC_BTN_COLOR_MIDDLE: return clickshow::kMiddle;
    case IDC_BTN_COLOR_RIGHT: return clickshow::kRight;
    case IDC_BTN_COLOR_X1: return clickshow::kX1;
    case IDC_BTN_COLOR_X2: return clickshow::kX2;
    default: return -1;
    }
}

int CheckboxButtonIndex(UINT controlId) {
    switch (controlId) {
    case IDC_CHK_BTN_LEFT: return clickshow::kLeft;
    case IDC_CHK_BTN_MIDDLE: return clickshow::kMiddle;
    case IDC_CHK_BTN_RIGHT: return clickshow::kRight;
    case IDC_CHK_BTN_X1: return clickshow::kX1;
    case IDC_CHK_BTN_X2: return clickshow::kX2;
    default: return -1;
    }
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
        SetWindowTextW(window, L"ClickShow 1.5.0 (C++)");

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
            if (GetTickCount64() >= g_screenshotHideUntil) {
                g_screenshotHidden = false;
                g_overlay.RestoreAfterScreenshot();
                KillTimer(window, kTimerScreenshot);
            }
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
        std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lParam));
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

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        return TRUE;

    case WM_DESTROY:
        KillTimer(window, kTimerSave);
        KillTimer(window, kTimerCounts);
        KillTimer(window, kTimerScreenshot);
        KillTimer(window, clickshow::kOverlayTimerId);

        // Match OnClosed: settings first, then stop input, then counts and tray.
        SaveSettingsOnly();
        g_rawInput.Stop();
        g_overlay.Stop();
        SaveCountsOnly();
        RemoveTrayIcon();
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

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"初始化 GDI+ 失败。", L"ClickShow", MB_OK | MB_ICONERROR);
        CoUninitialize();
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
        CoUninitialize();
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
            break;
        }
        if (!IsDialogMessageW(g_mainWindow, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    g_rawInput.Stop();
    g_overlay.Stop();
    RemoveTrayIcon();

    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    if (instanceMutex != nullptr) {
        CloseHandle(instanceMutex);
    }
    return exitCode;
}
