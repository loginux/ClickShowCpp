#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clickshow::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::wstring text;
    std::vector<std::unique_ptr<Value>> array;
    std::vector<std::pair<std::wstring, std::unique_ptr<Value>>> object;

    Value() = default;
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    bool IsNull() const { return type == Type::Null; }
    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }

    bool AsBool(bool fallback = false) const;
    std::wstring AsString(const std::wstring& fallback = L"") const;
    double AsDouble(double fallback = 0.0) const;
    long long AsInt64(long long fallback = 0) const;
    unsigned long long AsUInt64(unsigned long long fallback = 0) const;

    const Value* Find(const std::wstring& key) const;
};

Value Parse(const std::wstring& text, std::wstring* error = nullptr);
std::wstring Escape(const std::wstring& text);

}
