#include <windows.h>
#include <cstdio>
#include <string>

#include "settings.h"
#include "counts.h"
#include "util.h"

namespace {

const wchar_t* ButtonName(int index) {
    switch (index) {
    case 0: return L"Left";
    case 1: return L"Middle";
    case 2: return L"Right";
    case 3: return L"X1";
    case 4: return L"X2";
    default: return L"?";
    }
}

void PrintSettings(const wchar_t* label, const clickshow::Settings& settings) {
    wprintf(L"%s: circle=%d hover=%d count=%d indicator=%d hoverSize=%d hoverFill=#%02X%02X%02X alpha=%02X last=%s\n",
            label,
            settings.enableClickCircle ? 1 : 0,
            settings.enableHoverDot ? 1 : 0,
            settings.enableClickCount ? 1 : 0,
            settings.indicatorSize,
            settings.hoverDotSize,
            GetRValue(settings.hoverDotColor),
            GetGValue(settings.hoverDotColor),
            GetBValue(settings.hoverDotColor),
            settings.hoverDotAlpha,
            settings.lastNotifiedVersion.c_str());

    for (int index = 0; index < clickshow::kButtonCount; ++index) {
        const clickshow::ButtonSetting& button = settings.buttons[index];
        wprintf(L"  %s: enabled=%d color=#%02X%02X%02X alpha=%02X\n",
                ButtonName(index),
                button.enabled ? 1 : 0,
                GetRValue(button.color),
                GetGValue(button.color),
                GetBValue(button.color),
                button.alpha);
    }
}

}

int wmain() {
    clickshow::SettingsStore store;
    const clickshow::Settings settings = store.Load();
    wprintf(L"settings path: %s\n", store.Path().c_str());
    PrintSettings(L"loaded", settings);

    clickshow::CountStore counts;
    counts.Load();
    wprintf(L"counts: Left=%llu Middle=%llu Right=%llu X1=%llu X2=%llu Total=%llu\n",
            static_cast<unsigned long long>(counts.Get(clickshow::kLeft)),
            static_cast<unsigned long long>(counts.Get(clickshow::kMiddle)),
            static_cast<unsigned long long>(counts.Get(clickshow::kRight)),
            static_cast<unsigned long long>(counts.Get(clickshow::kX1)),
            static_cast<unsigned long long>(counts.Get(clickshow::kX2)),
            static_cast<unsigned long long>(counts.Total()));

    CreateDirectoryW(L"build_direct\\config_test", nullptr);
    clickshow::SettingsStore temp(L"build_direct\\config_test\\settings.json");
    const bool saved = temp.Save(settings);
    wprintf(L"roundtrip save: %d\n", saved ? 1 : 0);

    const clickshow::Settings roundtrip = temp.Load();
    PrintSettings(L"roundtrip", roundtrip);

    std::wstring json;
    if (clickshow::ReadFileUtf8(temp.Path(), json)) {
        wprintf(L"json: %s\n", json.c_str());
    }

    return 0;
}
