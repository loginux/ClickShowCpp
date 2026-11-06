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

struct ButtonSetting {
    bool enabled = true;
    COLORREF color = RGB(0, 0, 0);
    BYTE alpha = 0xFF;
};

struct Settings {
    bool enableClickCircle = true;
    bool enableHoverDot = false;
    bool enableClickCount = false;
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
    bool Save(const Settings& settings) const;
    bool ConsumeLoadError();
    const std::wstring& Path() const { return path_; }

private:
    bool LoadJson(Settings& settings) const;
    bool LoadIni(Settings& settings) const;
    bool SaveJson(const Settings& settings) const;
    bool SaveIni(const Settings& settings) const;

    std::wstring path_;
    std::wstring legacyPath_;
    bool loadError_ = false;
};

}
