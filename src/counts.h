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
    bool Save() const;
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
};

}
