#pragma once
#include <windows.h>
#include <string>

namespace clickshow {

std::wstring GetLocalAppDataPath();
std::wstring GetDocumentsPath();
std::wstring GetExePath();
bool ReadFileUtf8(const std::wstring& path, std::wstring& text);
bool WriteFileAtomicUtf8(const std::wstring& path, const std::wstring& text);
std::wstring Format(const wchar_t* fmt, ...);
std::wstring Trim(const std::wstring& text);
std::wstring Win32ErrorMessage(DWORD error);
void LogError(const wchar_t* source, const std::wstring& message);
int CompareVersions(const std::wstring& left, const std::wstring& right);
COLORREF ParseColor(const std::wstring& text, COLORREF fallback);
bool ParseArgb(const std::wstring& text, COLORREF& color, BYTE& alpha);
std::wstring ColorToHex(COLORREF color);
std::wstring ColorToArgb(COLORREF color, BYTE alpha);
UINT GetDpiForPoint(POINT point);
void InvalidateDpiCache();
UINT GetDpiForWindowSafe(HWND window);
bool SetWindowDisplayAffinitySafe(HWND window);
std::wstring Utf8ToWide(const std::string& text);

}
