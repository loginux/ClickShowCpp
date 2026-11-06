#include "settings.h"
#include "json.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

namespace clickshow {
namespace {

const wchar_t* const kButtonSections[kButtonCount] = {
    L"button.left",
    L"button.middle",
    L"button.right",
    L"button.x1",
    L"button.x2",
};

int MouseButtonIndexFromKey(long long key) {
    switch (key) {
    case 1048576LL: return kLeft;
    case 4194304LL: return kMiddle;
    case 2097152LL: return kRight;
    case 8388608LL: return kX1;
    case 16777216LL: return kX2;
    default: return -1;
    }
}

void Normalize(Settings& settings) {
    settings.indicatorSize = std::clamp(settings.indicatorSize, 50, 400);
    settings.hoverDotSize = std::clamp(settings.hoverDotSize, 10, 150);
}

}

Settings Settings::Defaults() {
    Settings settings;
    settings.enableClickCircle = true;
    settings.enableHoverDot = false;
    settings.enableClickCount = false;
    settings.indicatorSize = 150;
    settings.hoverDotSize = 60;
    settings.hoverDotColor = RGB(255, 255, 90);
    settings.hoverDotAlpha = 0x60;
    settings.buttons[kLeft] = ButtonSetting{true, RGB(30, 144, 255), 0xFF};
    settings.buttons[kMiddle] = ButtonSetting{true, RGB(0, 128, 0), 0xFF};
    settings.buttons[kRight] = ButtonSetting{true, RGB(255, 69, 0), 0xFF};
    settings.buttons[kX1] = ButtonSetting{true, RGB(128, 128, 128), 0xFF};
    settings.buttons[kX2] = ButtonSetting{true, RGB(138, 43, 226), 0xFF};
    return settings;
}

SettingsStore::SettingsStore() {
    path_ = GetDocumentsPath() + L"\\ClickShow.setting";
    legacyPath_ = GetLocalAppDataPath() + L"\\ClickShow\\settings.ini";
}

SettingsStore::SettingsStore(std::wstring path) : path_(std::move(path)) {
    legacyPath_ = path_ + L".ini";
}

Settings SettingsStore::Load() {
    loadError_ = false;
    Settings settings = Settings::Defaults();

    std::error_code error;
    const bool jsonExists = std::filesystem::exists(path_, error);
    if (jsonExists) {
        if (LoadJson(settings)) {
            Normalize(settings);
            return settings;
        }
        // The original app resets to defaults when ClickShow.setting is corrupt.
        loadError_ = true;
        Normalize(settings);
        return settings;
    }

    // Migration path for the first C++ version, which used an INI file.
    if (LoadIni(settings)) {
        Normalize(settings);
        return settings;
    }

    Normalize(settings);
    return settings;
}

bool SettingsStore::Save(const Settings& settings) const {
    if (SaveJson(settings)) {
        return true;
    }
    LogError(L"Settings.Save", L"写入 " + path_ + L" 失败，回退到 settings.ini");
    return SaveIni(settings);
}

bool SettingsStore::ConsumeLoadError() {
    const bool result = loadError_;
    loadError_ = false;
    return result;
}

bool SettingsStore::LoadJson(Settings& settings) const {
    std::wstring text;
    if (!ReadFileUtf8(path_, text)) {
        return false;
    }

    std::wstring error;
    json::Value root = json::Parse(text, &error);
    if (!error.empty() || !root.IsObject()) {
        LogError(L"Settings.LoadJson", L"解析 " + path_ + L" 失败：" + error);
        return false;
    }

    if (const auto* value = root.Find(L"EnableClickCircle")) {
        settings.enableClickCircle = value->AsBool(settings.enableClickCircle);
    }
    if (const auto* value = root.Find(L"EnableClickCount")) {
        settings.enableClickCount = value->AsBool(settings.enableClickCount);
    }
    if (const auto* value = root.Find(L"EnableHoverDot")) {
        settings.enableHoverDot = value->AsBool(settings.enableHoverDot);
    }
    if (const auto* value = root.Find(L"HoverDotFill")) {
        COLORREF color = settings.hoverDotColor;
        BYTE alpha = settings.hoverDotAlpha;
        if (ParseArgb(value->AsString(), color, alpha)) {
            settings.hoverDotColor = color;
            settings.hoverDotAlpha = alpha;
        }
    }
    if (const auto* value = root.Find(L"HoverDotSize")) {
        settings.hoverDotSize = static_cast<int>(std::lround(value->AsDouble(settings.hoverDotSize)));
    }
    if (const auto* value = root.Find(L"IndicatorSize")) {
        settings.indicatorSize = static_cast<int>(std::lround(value->AsDouble(settings.indicatorSize)));
    }
    if (const auto* value = root.Find(L"LastNotifiedVersion")) {
        settings.lastNotifiedVersion = value->AsString();
    }

    if (const auto* buttons = root.Find(L"MouseButtonSettings"); buttons != nullptr && buttons->IsArray()) {
        for (const auto& item : buttons->array) {
            if (item == nullptr || !item->IsObject()) {
                continue;
            }
            const auto* key = item->Find(L"Key");
            if (key == nullptr) {
                continue;
            }
            const int index = MouseButtonIndexFromKey(key->AsInt64(-1));
            if (index < 0) {
                continue;
            }
            const auto* value = item->Find(L"Value");
            if (value == nullptr || !value->IsObject()) {
                continue;
            }
            if (const auto* enabled = value->Find(L"IsEnabled")) {
                settings.buttons[index].enabled = enabled->AsBool(settings.buttons[index].enabled);
            }
            if (const auto* colorValue = value->Find(L"Color")) {
                COLORREF color = settings.buttons[index].color;
                BYTE alpha = settings.buttons[index].alpha;
                if (ParseArgb(colorValue->AsString(), color, alpha)) {
                    settings.buttons[index].color = color;
                    settings.buttons[index].alpha = alpha;
                }
            }
        }
    }

    Normalize(settings);
    return true;
}

bool SettingsStore::SaveJson(const Settings& settings) const {
    std::wstring out;
    out.reserve(1024);

    out += L"{\"EnableClickCircle\":";
    out += settings.enableClickCircle ? L"true" : L"false";
    out += L",\"EnableClickCount\":";
    out += settings.enableClickCount ? L"true" : L"false";
    out += L",\"EnableHoverDot\":";
    out += settings.enableHoverDot ? L"true" : L"false";
    out += L",\"HoverDotFill\":\"";
    out += ColorToArgb(settings.hoverDotColor, settings.hoverDotAlpha);
    out += L"\",\"HoverDotSize\":";
    out += std::to_wstring(settings.hoverDotSize);
    out += L",\"IndicatorSize\":";
    out += std::to_wstring(settings.indicatorSize);
    out += L",\"LastNotifiedVersion\":";
    if (settings.lastNotifiedVersion.empty()) {
        out += L"null";
    } else {
        out += L"\"";
        out += json::Escape(settings.lastNotifiedVersion);
        out += L"\"";
    }

    out += L",\"MouseButtonSettings\":[";
    const int order[kButtonCount] = {kLeft, kMiddle, kRight, kX1, kX2};
    const long long keys[kButtonCount] = {1048576LL, 4194304LL, 2097152LL, 8388608LL, 16777216LL};
    for (int index = 0; index < kButtonCount; ++index) {
        if (index > 0) {
            out += L",";
        }
        const ButtonSetting& button = settings.buttons[order[index]];
        out += L"{\"Key\":";
        out += std::to_wstring(keys[index]);
        out += L",\"Value\":{\"Color\":\"";
        out += ColorToArgb(button.color, button.alpha);
        out += L"\",\"IsEnabled\":";
        out += button.enabled ? L"true" : L"false";
        out += L"}}";
    }
    out += L"]}";

    if (!WriteFileAtomicUtf8(path_, out)) {
        LogError(L"Settings.SaveJson", L"写入 " + path_ + L" 失败");
        return false;
    }
    return true;
}

bool SettingsStore::LoadIni(Settings& settings) const {
    std::error_code error;
    if (!std::filesystem::exists(legacyPath_, error)) {
        return false;
    }

    Settings loaded = Settings::Defaults();
    loaded.enableClickCircle =
        GetPrivateProfileIntW(L"general", L"enableClickCircle", 1, legacyPath_.c_str()) != 0;
    loaded.enableHoverDot =
        GetPrivateProfileIntW(L"general", L"enableHoverDot", 0, legacyPath_.c_str()) != 0;
    loaded.enableClickCount =
        GetPrivateProfileIntW(L"general", L"enableClickCount", 0, legacyPath_.c_str()) != 0;
    loaded.indicatorSize = std::clamp<int>(
        static_cast<int>(GetPrivateProfileIntW(L"general", L"indicatorSize", 150, legacyPath_.c_str())),
        50, 400);
    loaded.hoverDotSize = std::clamp<int>(
        static_cast<int>(GetPrivateProfileIntW(L"general", L"hoverDotSize", 60, legacyPath_.c_str())),
        10, 150);

    wchar_t buffer[256]{};
    GetPrivateProfileStringW(L"general", L"hoverDotColor", L"", buffer, ARRAYSIZE(buffer), legacyPath_.c_str());
    if (buffer[0] != L'\0') {
        loaded.hoverDotColor = ParseColor(buffer, loaded.hoverDotColor);
    }
    loaded.hoverDotAlpha = static_cast<BYTE>(std::clamp<int>(
        static_cast<int>(GetPrivateProfileIntW(L"general", L"hoverDotAlpha", 0x60, legacyPath_.c_str())),
        0, 255));

    GetPrivateProfileStringW(L"general", L"lastNotifiedVersion", L"", buffer, ARRAYSIZE(buffer),
                             legacyPath_.c_str());
    loaded.lastNotifiedVersion = buffer;

    for (int index = 0; index < kButtonCount; ++index) {
        loaded.buttons[index].enabled =
            GetPrivateProfileIntW(kButtonSections[index], L"enabled", 1, legacyPath_.c_str()) != 0;
        buffer[0] = L'\0';
        GetPrivateProfileStringW(kButtonSections[index], L"color", L"", buffer, ARRAYSIZE(buffer),
                                 legacyPath_.c_str());
        if (buffer[0] != L'\0') {
            loaded.buttons[index].color = ParseColor(buffer, loaded.buttons[index].color);
        }
        loaded.buttons[index].alpha = 0xFF;
    }

    settings = std::move(loaded);
    return true;
}

bool SettingsStore::SaveIni(const Settings& settings) const {
    if (legacyPath_.empty()) {
        return false;
    }

    const auto parent = std::filesystem::path(legacyPath_).parent_path();
    std::error_code error;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
    }

    bool ok = true;
    auto putInt = [&](const wchar_t* section, const wchar_t* key, int value) {
        ok = (WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(),
                                         legacyPath_.c_str()) != FALSE) && ok;
    };
    auto putString = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        ok = (WritePrivateProfileStringW(section, key, value.c_str(), legacyPath_.c_str()) != FALSE) && ok;
    };

    putInt(L"general", L"enableClickCircle", settings.enableClickCircle ? 1 : 0);
    putInt(L"general", L"enableHoverDot", settings.enableHoverDot ? 1 : 0);
    putInt(L"general", L"enableClickCount", settings.enableClickCount ? 1 : 0);
    putInt(L"general", L"indicatorSize", settings.indicatorSize);
    putInt(L"general", L"hoverDotSize", settings.hoverDotSize);
    putString(L"general", L"hoverDotColor", ColorToHex(settings.hoverDotColor));
    putInt(L"general", L"hoverDotAlpha", settings.hoverDotAlpha);
    putString(L"general", L"lastNotifiedVersion", settings.lastNotifiedVersion);

    for (int index = 0; index < kButtonCount; ++index) {
        putInt(kButtonSections[index], L"enabled", settings.buttons[index].enabled ? 1 : 0);
        putString(kButtonSections[index], L"color", ColorToHex(settings.buttons[index].color));
    }

    return ok;
}

}
