#pragma once
#include <windows.h>
#include <array>
#include <string>

namespace clickshow {

enum ButtonIndex : int {
    kLeft = 0,
    kMiddle = 1,
    kRight = 2,
    kX1 = 3,
    kX2 = 4,
    kButtonCount = 5
};

// Value of the "Key" field used by both ClickShow.setting and ClickShow.counts.
inline constexpr long long kMouseButtonKeys[kButtonCount] = {
    1048576LL,  // Left
    4194304LL,  // Middle
    2097152LL,  // Right
    8388608LL,  // XButton1
    16777216LL, // XButton2
};

// The two files use different array orders; keep both here so the mapping has
// exactly one definition.
inline constexpr int kSettingsButtonOrder[kButtonCount] = {kLeft, kMiddle, kRight, kX1, kX2};
inline constexpr int kCountsSaveOrder[kButtonCount] = {kLeft, kRight, kMiddle, kX1, kX2};
inline constexpr long long kCountsSaveKeys[kButtonCount] = {
    1048576LL, 2097152LL, 4194304LL, 8388608LL, 16777216LL
};

int MouseButtonIndexFromKey(long long key);

// Distinguishes "the file is fine" from "the file is broken" and "we could not
// read it". Only a genuinely broken file may be replaced with defaults.
enum class LoadStatus { Ok, Unavailable, Corrupt };

struct ButtonSetting {
    bool enabled = true;
    COLORREF color = RGB(0, 0, 0);
    BYTE alpha = 0xFF;
};

struct Settings {
    bool enableClickCircle = true;
    bool enableHoverDot = false;
    bool enableClickCount = false;
    // Hide the hover dot while another application covers the whole screen,
    // which is what screenshot tools do while you pick a region.
    bool hideDotDuringCapture = true;
    int indicatorSize = 150;
    int hoverDotSize = 60;
    COLORREF hoverDotColor = RGB(255, 255, 90);
    BYTE hoverDotAlpha = 0x60;
    std::array<ButtonSetting, kButtonCount> buttons{};
    std::wstring lastNotifiedVersion;

    static Settings Defaults();
};

class SettingsStore {
public:
    SettingsStore();
    explicit SettingsStore(std::wstring path);

    Settings Load();
    bool Save(const Settings& settings);
    bool ConsumeLoadError();
    const std::wstring& Path() const { return path_; }

private:
    LoadStatus LoadJson(Settings& settings) const;
    bool HasPendingLegacyFallback() const;
    void ClearPendingLegacyFallback() const;
    bool LoadIni(Settings& settings) const;
    bool SaveJson(const Settings& settings) const;
    bool SaveIni(const Settings& settings) const;

    std::wstring path_;
    std::wstring legacyPath_;
    bool loadError_ = false;
    bool writeBlocked_ = false;
};

}
