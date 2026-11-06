#include "util.h"

#include <objbase.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <vector>

namespace clickshow {
namespace {

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring NowStamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
               static_cast<int>(time.wYear), static_cast<int>(time.wMonth), static_cast<int>(time.wDay),
               static_cast<int>(time.wHour), static_cast<int>(time.wMinute), static_cast<int>(time.wSecond),
               static_cast<int>(time.wMilliseconds));
    return buffer;
}

}

std::wstring GetLocalAppDataPath() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) && raw != nullptr) {
        std::wstring result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    return L".";
}

std::wstring GetDocumentsPath() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw)) && raw != nullptr) {
        std::wstring result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    return L".";
}

std::wstring GetExePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32768) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool ReadFileUtf8(const std::wstring& path, std::wstring& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        return false;
    }

    bytes.resize(read);
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    text = Utf8ToWide(bytes);
    return true;
}

bool WriteFileAtomicUtf8(const std::wstring& path, const std::wstring& text) {
    std::filesystem::path target(path);
    std::error_code error;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), error);
    }

    const std::string bytes = WideToUtf8(text);
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);

    if (!ok || written != bytes.size()) {
        DeleteFileW(temp.c_str());
        return false;
    }

    // Config files are tiny; a plain rename is atomic enough. Write-through
    // would force a physical flush on every save, which is unnecessary here.
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[1024]{};
    va_list args;
    va_start(args, fmt);
    int written = _vsnwprintf_s(buffer, _TRUNCATE, fmt, args);
    va_end(args);
    if (written < 0) {
        return {};
    }
    return std::wstring(buffer, static_cast<size_t>(written));
}

std::wstring Trim(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && iswspace(text[begin])) {
        ++begin;
    }
    if (begin < end && text[begin] == 0xFEFF) {
        ++begin;
    }
    while (end > begin && iswspace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::wstring Win32ErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result;
    if (length > 0 && buffer != nullptr) {
        result = Trim(std::wstring(buffer, length));
    } else {
        result = Format(L"Windows error %lu", static_cast<unsigned long>(error));
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return result;
}

void LogError(const wchar_t* source, const std::wstring& message) {
    const std::wstring line = L"[" + NowStamp() + L"] " + (source != nullptr ? source : L"?") + L"\r\n" + message + L"\r\n\r\n";
    OutputDebugStringW(line.c_str());

    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);

    const std::wstring directory = GetLocalAppDataPath() + L"\\ClickShow";
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring path = directory + L"\\error.log";

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        ULARGE_INTEGER size{};
        size.LowPart = attributes.nFileSizeLow;
        size.HighPart = attributes.nFileSizeHigh;
        if (size.QuadPart > 1024ull * 1024ull) {
            MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    const std::string utf8 = WideToUtf8(line);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(file);
    }
}

int CompareVersions(const std::wstring& left, const std::wstring& right) {
    size_t i = 0;
    size_t j = 0;
    while (i < left.size() || j < right.size()) {
        unsigned long a = 0;
        unsigned long b = 0;
        while (i < left.size() && left[i] != L'.') {
            if (iswdigit(left[i])) {
                a = a * 10 + static_cast<unsigned long>(left[i] - L'0');
            }
            ++i;
        }
        while (j < right.size() && right[j] != L'.') {
            if (iswdigit(right[j])) {
                b = b * 10 + static_cast<unsigned long>(right[j] - L'0');
            }
            ++j;
        }
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        if (i < left.size() && left[i] == L'.') {
            ++i;
        }
        if (j < right.size() && right[j] == L'.') {
            ++j;
        }
        if (i >= left.size() && j >= right.size()) {
            break;
        }
    }
    return 0;
}

bool ParseArgb(const std::wstring& text, COLORREF& color, BYTE& alpha) {
    std::wstring value = Trim(text);
    if (!value.empty() && value[0] == L'#') {
        value.erase(0, 1);
    }
    if (value.size() != 6 && value.size() != 8) {
        return false;
    }
    for (wchar_t ch : value) {
        if (!iswxdigit(ch)) {
            return false;
        }
    }

    wchar_t* end = nullptr;
    const unsigned long packed = wcstoul(value.c_str(), &end, 16);
    if (end == nullptr || *end != L'\0') {
        return false;
    }

    color = RGB((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF);
    alpha = value.size() == 8 ? static_cast<BYTE>((packed >> 24) & 0xFF) : 0xFF;
    return true;
}

COLORREF ParseColor(const std::wstring& text, COLORREF fallback) {
    COLORREF color = fallback;
    BYTE alpha = 0xFF;
    return ParseArgb(text, color, alpha) ? color : fallback;
}

std::wstring ColorToHex(COLORREF color) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}

std::wstring ColorToArgb(COLORREF color, BYTE alpha) {
    wchar_t buffer[12]{};
    swprintf_s(buffer, L"#%02X%02X%02X%02X", alpha, GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}

UINT GetDpiForPoint(POINT point) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn getDpiForMonitor = []() -> GetDpiForMonitorFn {
        HMODULE module = LoadLibraryW(L"Shcore.dll");
        if (module == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(module, "GetDpiForMonitor"));
    }();

    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (getDpiForMonitor != nullptr &&
        SUCCEEDED(getDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) &&
        dpiX >= 48) {
        return dpiX;
    }
    return GetDpiForWindowSafe(GetDesktopWindow());
}

UINT GetDpiForWindowSafe(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn getDpiForWindow = []() -> GetDpiForWindowFn {
        HMODULE module = GetModuleHandleW(L"user32.dll");
        if (module == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(module, "GetDpiForWindow"));
    }();

    if (getDpiForWindow != nullptr && window != nullptr) {
        UINT dpi = getDpiForWindow(window);
        if (dpi >= 48) {
            return dpi;
        }
    }

    HDC dc = GetDC(nullptr);
    int dpi = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc != nullptr) {
        ReleaseDC(nullptr, dc);
    }
    return dpi >= 48 ? static_cast<UINT>(dpi) : 96;
}

void SetWindowDisplayAffinitySafe(HWND window) {
    if (window == nullptr) {
        return;
    }
    constexpr DWORD kExcludeFromCapture = 0x00000011;
    SetWindowDisplayAffinity(window, kExcludeFromCapture);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

}
