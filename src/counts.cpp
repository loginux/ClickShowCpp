#include "counts.h"
#include "input.h"      // PhysicalKey: numpad storage IDs
#include "settings.h"   // kButtonCount, key tables, MouseButtonIndexFromKey
#include "json.h"
#include "util.h"

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>

namespace clickshow {
namespace {

const wchar_t* const kIniKeys[kButtonCount] = {L"left", L"middle", L"right", L"x1", L"x2"};
constexpr wchar_t kFallbackSection[] = L"fallback";
constexpr wchar_t kFallbackKey[] = L"pending";

bool HasPendingLegacyFallback(const std::wstring& legacyPath) {
    if (legacyPath.empty()) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::exists(legacyPath, error)) {
        return false;
    }
    return GetPrivateProfileIntW(kFallbackSection, kFallbackKey, 0, legacyPath.c_str()) != 0;
}

void ClearPendingLegacyFallback(const std::wstring& legacyPath) {
    WritePrivateProfileStringW(kFallbackSection, kFallbackKey, L"0", legacyPath.c_str());
}

}

CountStore::CountStore() {
    path_ = GetLocalAppDataPath() + L"\\ClickShow\\ClickShow.counts";
    legacyPath_ = GetLocalAppDataPath() + L"\\ClickShow\\counts.ini";
}

CountStore::CountStore(std::wstring path) : path_(std::move(path)) {
    legacyPath_ = path_ + L".ini";
}

void CountStore::Load() {
    counts_.fill(0);
    loadFailed_ = false;

    // A failed JSON save leaves a pending INI fallback. Recover it first,
    // otherwise the fallback would never be read while a stale JSON exists.
    if (HasPendingLegacyFallback(legacyPath_)) {
        if (LoadIni() && SaveJson()) {
            ClearPendingLegacyFallback(legacyPath_);
        } else {
            // Do not let the periodic save overwrite the real file with the
            // zeros we failed to replace.
            loadFailed_ = true;
            LogError(L"Counts.LoadJson", L"读取 INI 兜底 " + legacyPath_ + L" 失败，本次不写回计数");
        }
        return;
    }

    std::error_code error;
    const bool jsonExists = std::filesystem::exists(path_, error);
    if (error) {
        loadFailed_ = true;
        LogError(L"Counts.LoadJson", L"无法检查 " + path_ + L"，本次不写回计数");
        return;
    }
    if (jsonExists) {
        if (!LoadJson()) {
            // Keep the file intact: an unreadable/corrupt counts file must not
            // be silently overwritten by the next periodic save.
            loadFailed_ = true;
            LogError(L"Counts.LoadJson", L"读取或解析 " + path_ + L" 失败，本次不写回计数");
        }
        return;
    }

    LoadIni();
}

bool CountStore::Save() {
    if (loadFailed_) {
        LogError(L"Counts.Save", L"启动时计数文件不可用，跳过写回以免覆盖原文件");
        return false;
    }
    if (SaveJson()) {
        // JSON is healthy again. Clear a stale pending marker left by an
        // earlier INI fallback so the next launch reads this newer JSON instead
        // of restoring the old INI snapshot over it. Guard with exists() so a
        // successful save does not create an otherwise-unused INI file.
        if (HasPendingLegacyFallback(legacyPath_)) {
            ClearPendingLegacyFallback(legacyPath_);
        }
        return true;
    }
    LogError(L"Counts.SaveJson", L"写入 " + path_ + L" 失败，回退到 counts.ini");
    return SaveIni();
}

bool CountStore::LoadJson() {
    std::wstring text;
    if (!ReadFileUtf8(path_, text)) {
        return false;
    }

    std::wstring error;
    json::Value root = json::Parse(text, &error);
    if (!error.empty() || !root.IsObject()) {
        LogError(L"Counts.LoadJson", L"解析失败：" + error);
        return false;
    }

    const auto* array = root.Find(L"Counts");
    if (array == nullptr || !array->IsArray()) {
        LogError(L"Counts.LoadJson", L"缺少 Counts 数组");
        return false;
    }

    for (const auto& item : array->array) {
        if (item == nullptr || !item->IsObject()) {
            continue;
        }
        const auto* key = item->Find(L"Key");
        const auto* value = item->Find(L"Value");
        if (key == nullptr || value == nullptr) {
            continue;
        }
        const int index = clickshow::MouseButtonIndexFromKey(key->AsInt64(-1));
        if (index >= 0) {
            counts_[index] = value->AsUInt64(0);
        }
    }
    return true;
}

bool CountStore::SaveJson() const {
    std::wstring out = L"{\"Counts\":[";
    for (int index = 0; index < kButtonCount; ++index) {
        if (index > 0) {
            out += L",";
        }
        out += L"{\"Key\":";
        out += std::to_wstring(kCountsSaveKeys[index]);
        out += L",\"Value\":";
        out += std::to_wstring(counts_[kCountsSaveOrder[index]]);
        out += L"}";
    }
    out += L"]}";

    if (!WriteFileAtomicUtf8(path_, out)) {
        LogError(L"Counts.SaveJson", L"写入 " + path_ + L" 失败");
        return false;
    }
    return true;
}

bool CountStore::LoadIni() {
    std::error_code error;
    if (!std::filesystem::exists(legacyPath_, error)) {
        return false;
    }

    for (int index = 0; index < kButtonCount; ++index) {
        wchar_t buffer[64]{};
        GetPrivateProfileStringW(L"counts", kIniKeys[index], L"0", buffer, ARRAYSIZE(buffer),
                                 legacyPath_.c_str());
        counts_[index] = _wcstoui64(buffer, nullptr, 10);
    }
    return true;
}

bool CountStore::SaveIni() const {
    if (legacyPath_.empty()) {
        return false;
    }

    const auto parent = std::filesystem::path(legacyPath_).parent_path();
    std::error_code error;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
    }

    bool ok = true;
    for (int index = 0; index < kButtonCount; ++index) {
        ok = (WritePrivateProfileStringW(L"counts", kIniKeys[index],
                                         std::to_wstring(counts_[index]).c_str(),
                                         legacyPath_.c_str()) != FALSE) && ok;
    }
    if (ok) {
        ok = WritePrivateProfileStringW(kFallbackSection, kFallbackKey, L"1",
                                        legacyPath_.c_str()) != FALSE;
    }
    return ok;
}

void CountStore::Reset() {
    counts_.fill(0);
    // An explicit reset is a user request: allow writing the file again.
    loadFailed_ = false;
}

void CountStore::Add(int button) {
    if (button >= 0 && button < kButtonCount) {
        ++counts_[button];
    }
}

uint64_t CountStore::Get(int button) const {
    return (button >= 0 && button < kButtonCount) ? counts_[button] : 0;
}

uint64_t CountStore::Total() const {
    uint64_t total = 0;
    for (uint64_t value : counts_) {
        total += value;
    }
    return total;
}

// --- KeyCountStore -------------------------------------------------------

KeyCountStore::KeyCountStore() {
    path_ = GetLocalAppDataPath() + L"\\ClickShow\\ClickShow.keycounts";
}

KeyCountStore::KeyCountStore(std::wstring path) : path_(std::move(path)) {}

void KeyCountStore::Load() {
    counts_.fill(0);
    loadFailed_ = false;

    std::wstring text;
    if (!ReadFileUtf8(path_, text)) {
        // Only a genuinely absent file means "no history yet". If the file
        // exists but could not be read (locked, permissions, >16MB), keep the
        // in-memory zeros but block writes so the periodic save cannot wipe the
        // real file. This matches CountStore::Load()'s protect-the-file policy.
        std::error_code error;
        const bool exists = std::filesystem::exists(path_, error);
        if (error || exists) {
            loadFailed_ = true;
            LogError(L"KeyCounts.Load", L"读取或检查 " + path_ + L" 失败，本次不写回");
        }
        return;
    }

    std::wstring error;
    json::Value root = json::Parse(text, &error);
    if (!error.empty() || !root.IsObject()) {
        loadFailed_ = true;
        LogError(L"KeyCounts.Load", L"解析 " + path_ + L" 失败，本次不写回");
        return;
    }
    const auto* keys = root.Find(L"Keys");
    if (keys == nullptr || !keys->IsArray()) {
        loadFailed_ = true;
        LogError(L"KeyCounts.Load", L"缺少 Keys 数组，本次不写回");
        return;
    }
    for (const auto& item : keys->array) {
        if (item == nullptr || !item->IsObject()) {
            continue;
        }
        const auto* key = item->Find(L"Key");
        const auto* value = item->Find(L"Value");
        if (key == nullptr || value == nullptr) {
            continue;
        }
        const long long vkey = key->AsInt64(-1);
        if (vkey >= 0 && vkey < kVirtualKeyCount) {
            counts_[static_cast<size_t>(vkey)] = value->AsUInt64(0);
        }
    }

    // 1.54 stored numpad keys under their Windows VKs. Fold those counts into
    // the physical-key IDs used from 1.55 so existing history stays visible.
    // VK_RETURN is intentionally left alone: it cannot distinguish main Enter
    // from numpad Enter.
    struct NumpadMigration {
        BYTE from;
        BYTE to;
    };
    constexpr NumpadMigration migrations[] = {
        {VK_NUMPAD0, static_cast<BYTE>(PhysicalKey::Numpad0)},
        {VK_NUMPAD1, static_cast<BYTE>(PhysicalKey::Numpad1)},
        {VK_NUMPAD2, static_cast<BYTE>(PhysicalKey::Numpad2)},
        {VK_NUMPAD3, static_cast<BYTE>(PhysicalKey::Numpad3)},
        {VK_NUMPAD4, static_cast<BYTE>(PhysicalKey::Numpad4)},
        {VK_NUMPAD5, static_cast<BYTE>(PhysicalKey::Numpad5)},
        {VK_NUMPAD6, static_cast<BYTE>(PhysicalKey::Numpad6)},
        {VK_NUMPAD7, static_cast<BYTE>(PhysicalKey::Numpad7)},
        {VK_NUMPAD8, static_cast<BYTE>(PhysicalKey::Numpad8)},
        {VK_NUMPAD9, static_cast<BYTE>(PhysicalKey::Numpad9)},
        {VK_MULTIPLY, static_cast<BYTE>(PhysicalKey::NumpadMultiply)},
        {VK_ADD, static_cast<BYTE>(PhysicalKey::NumpadAdd)},
        {VK_SUBTRACT, static_cast<BYTE>(PhysicalKey::NumpadSubtract)},
        {VK_DECIMAL, static_cast<BYTE>(PhysicalKey::NumpadDecimal)},
        {VK_DIVIDE, static_cast<BYTE>(PhysicalKey::NumpadDivide)},
        {VK_NUMLOCK, static_cast<BYTE>(PhysicalKey::NumLock)},
    };
    for (const NumpadMigration& item : migrations) {
        uint64_t& from = counts_[static_cast<size_t>(item.from)];
        uint64_t& to = counts_[static_cast<size_t>(item.to)];
        to += from;
        from = 0;
    }
}

bool KeyCountStore::Save() {
    if (loadFailed_) {
        LogError(L"KeyCounts.Save", L"启动时键盘计数文件不可用，跳过写回");
        return false;
    }

    std::wstring out = L"{\"Keys\":[";
    bool first = true;
    for (int vkey = 0; vkey < kVirtualKeyCount; ++vkey) {
        if (counts_[static_cast<size_t>(vkey)] == 0) {
            continue;
        }
        if (!first) {
            out += L",";
        }
        first = false;
        out += L"{\"Key\":";
        out += std::to_wstring(vkey);
        out += L",\"Value\":";
        out += std::to_wstring(counts_[static_cast<size_t>(vkey)]);
        out += L"}";
    }
    out += L"]}";

    if (!WriteFileAtomicUtf8(path_, out)) {
        LogError(L"KeyCounts.Save", L"写入 " + path_ + L" 失败");
        return false;
    }
    return true;
}

void KeyCountStore::Reset() {
    counts_.fill(0);
    loadFailed_ = false;
}

void KeyCountStore::Add(uint8_t vkey) {
    ++counts_[static_cast<size_t>(vkey)];
}

uint64_t KeyCountStore::Get(uint8_t vkey) const {
    return counts_[static_cast<size_t>(vkey)];
}

uint64_t KeyCountStore::Total() const {
    uint64_t total = 0;
    for (uint64_t value : counts_) {
        total += value;
    }
    return total;
}

}
