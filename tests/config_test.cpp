#include <windows.h>
#include <cstdio>
#include <string>

#include "settings.h"
#include "counts.h"
#include "json.h"
#include "util.h"

namespace {

int g_failures = 0;

void Check(bool ok, const wchar_t* what, int line) {
    if (!ok) {
        wprintf(L"FAIL (line %d): %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expression, description) Check((expression), (description), __LINE__)

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

void TestSettingsRoundtrip(const std::wstring& dir) {
    const std::wstring path = dir + L"\\settings.json";
    DeleteFileW((path + L".ini").c_str());
    clickshow::SettingsStore store(path);

    clickshow::Settings settings = clickshow::Settings::Defaults();
    settings.enableClickCircle = false;
    settings.enableHoverDot = true;
    settings.enableClickCount = true;
    settings.hideDotDuringCapture = false;
    settings.indicatorSize = 275;
    settings.hoverDotSize = 33;
    settings.hoverDotColor = RGB(0x12, 0x34, 0x56);
    settings.hoverDotAlpha = 0x77;
    settings.buttons[clickshow::kLeft].enabled = false;
    settings.buttons[clickshow::kLeft].color = RGB(1, 2, 3);
    settings.buttons[clickshow::kLeft].alpha = 0x40;
    settings.lastNotifiedVersion = L"1.51|https://example.invalid/a";

    CHECK(store.Save(settings), L"saving settings succeeds");
    std::wstring json;
    CHECK(clickshow::ReadFileUtf8(path, json), L"reading the saved JSON succeeds");
    CHECK(json.find(L"\"HoverDotFill\":\"#77123456\"") != std::wstring::npos,
          L"hover alpha is written as #AARRGGBB");
    CHECK(json.find(L"#40010203") != std::wstring::npos,
          L"per-button alpha is written as #AARRGGBB");

    const clickshow::Settings loaded = store.Load();
    CHECK(!store.ConsumeLoadError(), L"a written file is not reported as corrupt");
    CHECK(loaded.enableClickCircle == false && loaded.enableHoverDot == true &&
          loaded.enableClickCount == true, L"boolean settings round-trip");
    CHECK(loaded.hideDotDuringCapture == false,
          L"hideDotDuringCapture round-trips");
    CHECK(json.find(L"\"HideDotDuringCapture\":false") != std::wstring::npos,
          L"hideDotDuringCapture is written to the JSON");
    CHECK(loaded.indicatorSize == 275 && loaded.hoverDotSize == 33,
          L"size settings round-trip");
    CHECK(loaded.hoverDotColor == settings.hoverDotColor && loaded.hoverDotAlpha == 0x77,
          L"hover colour and alpha round-trip");
    CHECK(loaded.buttons[clickshow::kLeft].color == settings.buttons[clickshow::kLeft].color &&
          loaded.buttons[clickshow::kLeft].alpha == 0x40 &&
          loaded.buttons[clickshow::kLeft].enabled == false,
          L"per-button colour, alpha and enabled round-trip");
    CHECK(loaded.lastNotifiedVersion == settings.lastNotifiedVersion,
          L"lastNotifiedVersion round-trips (including the |url suffix)");

    CHECK(store.Save(loaded), L"second save succeeds");
    std::wstring second;
    CHECK(clickshow::ReadFileUtf8(path, second), L"reading the second JSON succeeds");
    CHECK(json == second, L"saving is byte-stable");
}

void TestOriginalJsonCompatibility(const std::wstring& dir) {
    const std::wstring path = dir + L"\\csharp.json";
    DeleteFileW((path + L".ini").c_str());
    // Different key order, an unknown field, and a nested Value object written
    // the way DataContractJsonSerializer emits it.
    CHECK(clickshow::WriteFileAtomicUtf8(path,
              L"{\"IndicatorSize\":88,\"EnableHoverDot\":true,\"UnknownField\":123,"
              L"\"MouseButtonSettings\":[{\"Value\":{\"Color\":\"#80112233\","
              L"\"IsEnabled\":false},\"Key\":1048576}]}"),
          L"writing the C# fixture succeeds");

    clickshow::SettingsStore store(path);
    const clickshow::Settings loaded = store.Load();
    CHECK(!store.ConsumeLoadError(), L"unknown fields do not mark the file corrupt");
    CHECK(loaded.indicatorSize == 88 && loaded.enableHoverDot,
          L"C# field order is parsed correctly");
    CHECK(loaded.buttons[clickshow::kLeft].color == RGB(0x11, 0x22, 0x33) &&
          loaded.buttons[clickshow::kLeft].alpha == 0x80 &&
          !loaded.buttons[clickshow::kLeft].enabled,
          L"nested button Value with #AARRGGBB is parsed");
}

void TestPendingIniFallback(const std::wstring& dir) {
    const std::wstring path = dir + L"\\fallback.json";
    const std::wstring ini = path + L".ini";
    DeleteFileW(ini.c_str());
    CHECK(clickshow::WriteFileAtomicUtf8(path, L"{\"IndicatorSize\":333}"),
          L"writing a stale JSON succeeds");
    CHECK(WritePrivateProfileStringW(L"general", L"indicatorSize", L"222", ini.c_str()) != FALSE,
          L"writing the INI fallback succeeds");
    CHECK(WritePrivateProfileStringW(L"fallback", L"pending", L"1", ini.c_str()) != FALSE,
          L"writing the pending marker succeeds");

    clickshow::SettingsStore store(path);
    const clickshow::Settings loaded = store.Load();
    CHECK(loaded.indicatorSize == 222,
          L"a pending INI fallback wins over the stale JSON file");

    std::wstring migrated;
    CHECK(clickshow::ReadFileUtf8(path, migrated), L"reading the migrated JSON succeeds");
    CHECK(migrated.find(L"\"IndicatorSize\":222") != std::wstring::npos,
          L"fallback data is migrated back into the JSON file");
    CHECK(GetPrivateProfileIntW(L"fallback", L"pending", -1, ini.c_str()) == 0,
          L"the pending marker is cleared after recovery");
}

void TestCountsRoundtrip(const std::wstring& dir) {
    const std::wstring path = dir + L"\\counts.json";
    DeleteFileW((path + L".ini").c_str());
    DeleteFileW(path.c_str()); // make the test idempotent across runs
    clickshow::CountStore counts(path);
    counts.Load();
    counts.Add(clickshow::kLeft);
    counts.Add(clickshow::kLeft);
    counts.Add(clickshow::kX2);
    CHECK(counts.Get(clickshow::kLeft) == 2 && counts.Total() == 3,
          L"Add/Get/Total work");
    CHECK(counts.Save(), L"saving counts succeeds");

    std::wstring json;
    CHECK(clickshow::ReadFileUtf8(path, json), L"reading the counts JSON succeeds");
    CHECK(json == L"{\"Counts\":[{\"Key\":1048576,\"Value\":2},"
                  L"{\"Key\":2097152,\"Value\":0},{\"Key\":4194304,\"Value\":0},"
                  L"{\"Key\":8388608,\"Value\":0},{\"Key\":16777216,\"Value\":1}]}",
          L"counts use the original key order");

    clickshow::CountStore reloaded(path);
    reloaded.Load();
    CHECK(reloaded.Get(clickshow::kLeft) == 2 && reloaded.Total() == 3,
          L"counts round-trip");
    reloaded.Reset();
    CHECK(reloaded.Total() == 0, L"Reset clears all counts");
}

void TestUnreadableFileIsNotOverwritten(const std::wstring& dir) {
    const std::wstring path = dir + L"\\blocked.json";
    // A directory at the JSON path makes it impossible to open as a file, which
    // is how a locked/permission-denied config behaves. It must NOT be treated
    // as corruption, and saving must be refused so the real file survives.
    RemoveDirectoryW(path.c_str());
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".ini").c_str());
    CHECK(CreateDirectoryW(path.c_str(), nullptr) != FALSE,
          L"creating the blocked path succeeds");

    clickshow::SettingsStore store(path);
    const clickshow::Settings loaded = store.Load();
    CHECK(!store.ConsumeLoadError(), L"an unreadable file is not reported as corrupt");
    CHECK(loaded.indicatorSize == 150, L"defaults are used while the file is unreadable");

    clickshow::Settings changed = loaded;
    changed.indicatorSize = 250;
    CHECK(!store.Save(changed), L"saving is refused while the file is unreadable");

    RemoveDirectoryW(path.c_str());
}

void TestVersionComparison() {
    // The update check compares the server string against kAppVersion, so the
    // two-component form used since 1.51 must order correctly.
    CHECK(clickshow::CompareVersions(L"1.51", L"1.5.0") > 0,
          L"1.51 is newer than 1.5.0");
    CHECK(clickshow::CompareVersions(L"1.5.0", L"1.51") < 0,
          L"1.5.0 is older than 1.51");
    CHECK(clickshow::CompareVersions(L"1.51", L"1.51") == 0,
          L"identical versions compare equal");
    CHECK(clickshow::CompareVersions(L"1.51.1", L"1.51") > 0,
          L"a trailing component counts as newer");
    CHECK(clickshow::CompareVersions(L"1.52", L"1.51") > 0,
          L"1.52 is newer than 1.51");
    // Segments are compared numerically, so 1.51 means minor=51 and is newer
    // than 1.6. Any future release must therefore be numbered 1.52 or higher.
    CHECK(clickshow::CompareVersions(L"1.6", L"1.51") < 0,
          L"1.6 compares older than 1.51 (minor 6 < 51)");
}

void TestJsonSafety() {
    std::wstring deep;
    for (int index = 0; index < 100; ++index) {
        deep += L"[";
    }
    std::wstring error;
    clickshow::json::Parse(deep, &error);
    CHECK(!error.empty(), L"deeply nested JSON is rejected instead of overflowing the stack");

    error.clear();
    const clickshow::json::Value value =
        clickshow::json::Parse(L"{\"s\":\"\\uD83D\\uDE00\"}", &error);
    CHECK(error.empty(), L"well-formed JSON parses");
    const clickshow::json::Value* text = value.Find(L"s");
    CHECK(text != nullptr && text->AsString() == L"\U0001F600",
          L"a \\u surrogate pair decodes to the right UTF-16 character");
}

} // namespace

int wmain() {
    // Informational: print the real config so C#/C++ compatibility can still
    // be eyeballed. The assertions below are self-contained and deterministic.
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

    CreateDirectoryW(L"build_direct", nullptr);
    CreateDirectoryW(L"build_direct\\config_test", nullptr);
    const std::wstring dir = L"build_direct\\config_test";

    TestSettingsRoundtrip(dir);
    TestOriginalJsonCompatibility(dir);
    TestPendingIniFallback(dir);
    TestUnreadableFileIsNotOverwritten(dir);
    TestCountsRoundtrip(dir);
    TestVersionComparison();
    TestJsonSafety();

    wprintf(L"\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
