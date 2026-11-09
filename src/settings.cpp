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

constexpr wchar_t kFallbackSection[] = L"fallback";
constexpr wchar_t kFallbackKey[] = L"pending";

void Normalize(Settings& settings) {
    settings.indicatorSize = std::clamp(settings.indicatorSize, 50, 400);
    settings.hoverDotSize = std::clamp(settings.hoverDotSize, 10, 150);
}

} // namespace

int MouseButtonIndexFromKey(long long key) {
    for (int index = 0; index < kButtonCount; ++index) {
        if (kMouseButtonKeys[index] == key) {
            return index;
        }
    }
    return -1;
}

Settings Settings::Defaults() {
    // All non-button defaults come from the member initializers in settings.h;
    // only the per-button colours need to be supplied here.
    Settings settings;
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
    writeBlocked_ = false;
    Settings settings = Settings::Defaults();

    // If a previous run could not write the JSON file, it left an INI fallback
    // with a "pending" marker. Recover from it and migrate back to JSON,
    // otherwise the fallback would never be read (Load only looks at the INI
    // when the JSON file is absent).
    if (HasPendingLegacyFallback()) {
        if (LoadIni(settings)) {
            Normalize(settings);
            if (SaveJson(settings)) {
                ClearPendingLegacyFallback();
            }
            return settings;
        }
    }

    std::error_code error;
    const bool jsonExists = std::filesystem::exists(path_, error);
    if (error) {
        // We could not even stat the file (permissions, offline profile...).
        // Treat it like an unreadable file: never write defaults over it.
        writeBlocked_ = true;
        Normalize(settings);
        return settings;
    }
    if (jsonExists) {
        const LoadStatus status = LoadJson(settings);
        if (status == LoadStatus::Corrupt) {
            // The original app resets to defaults when ClickShow.setting is
            // corrupt, and the caller then saves those defaults immediately.
            loadError_ = true;
        } else if (status == LoadStatus::Unavailable) {
            // Locked/transient I/O error: keep using defaults in memory but
            // never write them back, including on the normal exit save.
            writeBlocked_ = true;
        }
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

bool SettingsStore::HasPendingLegacyFallback() const {
    if (legacyPath_.empty()) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::exists(legacyPath_, error)) {
        return false;
    }
    return GetPrivateProfileIntW(kFallbackSection, kFallbackKey, 0, legacyPath_.c_str()) != 0;
}

void SettingsStore::ClearPendingLegacyFallback() const {
    WritePrivateProfileStringW(kFallbackSection, kFallbackKey, L"0", legacyPath_.c_str());
}

bool SettingsStore::Save(const Settings& settings) {
    if (writeBlocked_) {
        LogError(L"Settings.Save", L"启动时配置文件不可读，跳过写入以免覆盖原文件");
        return false;
    }
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

LoadStatus SettingsStore::LoadJson(Settings& settings) const {
    std::wstring text;
    if (!ReadFileUtf8(path_, text)) {
        LogError(L"Settings.LoadJson", L"读取 " + path_ + L" 失败，本次不覆盖文件");
        return LoadStatus::Unavailable;
    }

    std::wstring error;
    json::Value root = json::Parse(text, &error);
    if (!error.empty() || !root.IsObject()) {
        LogError(L"Settings.LoadJson", L"解析 " + path_ + L" 失败：" + error);
        return LoadStatus::Corrupt;
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

    return LoadStatus::Ok;
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
    for (int index = 0; index < kButtonCount; ++index) {
        if (index > 0) {
            out += L",";
        }
        const ButtonSetting& button = settings.buttons[kSettingsButtonOrder[index]];
        out += L"{\"Key\":";
        out += std::to_wstring(kMouseButtonKeys[kSettingsButtonOrder[index]]);
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
        // ParseArgb accepts both the old #RRGGBB form and the new #AARRGGBB one.
        COLORREF color = loaded.hoverDotColor;
        BYTE alpha = loaded.hoverDotAlpha;
        if (ParseArgb(buffer, color, alpha)) {
            loaded.hoverDotColor = color;
        } else {
            loaded.hoverDotColor = ParseColor(buffer, loaded.hoverDotColor);
        }
    }
    // Alpha has its own key, so a 6-digit legacy colour keeps the saved alpha.
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
            COLORREF color = loaded.buttons[index].color;
            BYTE alpha = loaded.buttons[index].alpha;
            if (ParseArgb(buffer, color, alpha)) {
                loaded.buttons[index].color = color;
            } else {
                loaded.buttons[index].color = ParseColor(buffer, loaded.buttons[index].color);
            }
        }
        loaded.buttons[index].alpha = static_cast<BYTE>(std::clamp<int>(
            static_cast<int>(GetPrivateProfileIntW(kButtonSections[index], L"alpha", 0xFF,
                                                   legacyPath_.c_str())),
            0, 255));
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
    putString(L"general", L"hoverDotColor",
              ColorToArgb(settings.hoverDotColor, settings.hoverDotAlpha));
    putInt(L"general", L"hoverDotAlpha", settings.hoverDotAlpha);
    putString(L"general", L"lastNotifiedVersion", settings.lastNotifiedVersion);

    for (int index = 0; index < kButtonCount; ++index) {
        putInt(kButtonSections[index], L"enabled", settings.buttons[index].enabled ? 1 : 0);
        putString(kButtonSections[index], L"color",
                  ColorToArgb(settings.buttons[index].color, settings.buttons[index].alpha));
        putInt(kButtonSections[index], L"alpha", settings.buttons[index].alpha);
    }

    if (ok) {
        // Marks the INI as a pending fallback so Load() knows to prefer it over
        // a stale JSON file on the next start.
        ok = WritePrivateProfileStringW(kFallbackSection, kFallbackKey, L"1",
                                        legacyPath_.c_str()) != FALSE;
    }
    return ok;
}

}
