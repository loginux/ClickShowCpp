#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace clickshow {

class CountStore {
public:
    CountStore();
    explicit CountStore(std::wstring path);

    void Load();
    bool Save();
    void Reset();
    void Add(int button);
    uint64_t Get(int button) const;
    uint64_t Total() const;

private:
    bool LoadJson();
    bool LoadIni();
    bool SaveJson() const;
    bool SaveIni() const;

    std::wstring path_;
    std::wstring legacyPath_;
    std::array<uint64_t, 5> counts_{};
    bool loadFailed_ = false;
};

// Per-virtual-key counts live in their own file so the mouse counts JSON keeps
// its original byte-for-byte format (the C# version must still read it).
constexpr int kVirtualKeyCount = 256;

class KeyCountStore {
public:
    KeyCountStore();
    explicit KeyCountStore(std::wstring path);

    void Load();
    bool Save();
    void Reset();
    void Add(uint8_t vkey);
    uint64_t Get(uint8_t vkey) const;
    uint64_t Total() const;

private:
    std::wstring path_;
    std::array<uint64_t, kVirtualKeyCount> counts_{};
    bool loadFailed_ = false;
};

}
