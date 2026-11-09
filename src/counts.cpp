#include "counts.h"
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

}
