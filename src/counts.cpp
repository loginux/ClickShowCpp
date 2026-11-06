#include "counts.h"
#include "json.h"
#include "util.h"

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>

namespace clickshow {
namespace {

const wchar_t* const kIniKeys[5] = {L"left", L"middle", L"right", L"x1", L"x2"};

int MouseButtonIndexFromKey(long long key) {
    switch (key) {
    case 1048576LL: return 0; // Left
    case 4194304LL: return 1; // Middle
    case 2097152LL: return 2; // Right
    case 8388608LL: return 3; // XButton1
    case 16777216LL: return 4; // XButton2
    default: return -1;
    }
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

    std::error_code error;
    const bool jsonExists = std::filesystem::exists(path_, error);
    if (jsonExists) {
        if (!LoadJson()) {
            LogError(L"Counts.LoadJson", L"解析 " + path_ + L" 失败，计数从 0 开始");
        }
        return;
    }

    LoadIni();
}

bool CountStore::Save() const {
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
        const int index = MouseButtonIndexFromKey(key->AsInt64(-1));
        if (index >= 0) {
            counts_[index] = value->AsUInt64(0);
        }
    }
    return true;
}

bool CountStore::SaveJson() const {
    std::wstring out = L"{\"Counts\":[";
    const int order[5] = {0, 2, 1, 3, 4}; // Left, Right, Middle, X1, X2 (original order)
    const long long keys[5] = {1048576LL, 2097152LL, 4194304LL, 8388608LL, 16777216LL};
    for (int index = 0; index < 5; ++index) {
        if (index > 0) {
            out += L",";
        }
        out += L"{\"Key\":";
        out += std::to_wstring(keys[index]);
        out += L",\"Value\":";
        out += std::to_wstring(counts_[order[index]]);
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

    for (int index = 0; index < 5; ++index) {
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
    for (int index = 0; index < 5; ++index) {
        ok = (WritePrivateProfileStringW(L"counts", kIniKeys[index],
                                         std::to_wstring(counts_[index]).c_str(),
                                         legacyPath_.c_str()) != FALSE) && ok;
    }
    return ok;
}

void CountStore::Reset() {
    counts_.fill(0);
}

void CountStore::Add(int button) {
    if (button >= 0 && button < 5) {
        ++counts_[button];
    }
}

uint64_t CountStore::Get(int button) const {
    return (button >= 0 && button < 5) ? counts_[button] : 0;
}

uint64_t CountStore::Total() const {
    uint64_t total = 0;
    for (uint64_t value : counts_) {
        total += value;
    }
    return total;
}

}
