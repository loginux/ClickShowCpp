#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

namespace clickshow::json {
namespace {

// A corrupt or hostile config file must not be able to blow the stack through
// deeply nested arrays/objects. Both files we parse are flat in practice.
constexpr int kMaxDepth = 64;

class DepthGuard {
public:
    explicit DepthGuard(int& depth) : depth_(depth), ok_(depth < kMaxDepth) {
        if (ok_) {
            ++depth;
        }
    }
    ~DepthGuard() {
        if (ok_) {
            --depth_;
        }
    }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
    bool ok() const { return ok_; }

private:
    int& depth_;
    bool ok_;
};

class Parser {
public:
    explicit Parser(const std::wstring& text) : text_(text) {}

    bool ParseValue(Value& out);
    void SkipWhitespace();
    size_t Position() const { return position_; }

    std::wstring error;

private:
    bool ParseObject(Value& out);
    bool ParseArray(Value& out);
    bool ParseString(std::wstring& out);
    bool ParseNumber(Value& out);
    bool ParseLiteral(const wchar_t* literal, Value& out, Value::Type type, bool boolean);
    bool ParseHex4(unsigned& out);
    bool Fail(const wchar_t* message);
    bool Consume(wchar_t ch);

    const std::wstring& text_;
    size_t position_ = 0;
    int depth_ = 0;
};

bool Parser::Fail(const wchar_t* message) {
    if (error.empty()) {
        error = std::wstring(message) + L" at offset " + std::to_wstring(position_);
    }
    return false;
}

bool Parser::Consume(wchar_t ch) {
    if (position_ < text_.size() && text_[position_] == ch) {
        ++position_;
        return true;
    }
    return false;
}

void Parser::SkipWhitespace() {
    while (position_ < text_.size() && iswspace(text_[position_])) {
        ++position_;
    }
}

bool Parser::ParseValue(Value& out) {
    DepthGuard guard(depth_);
    if (!guard.ok()) {
        return Fail(L"nesting too deep");
    }

    SkipWhitespace();
    if (position_ >= text_.size()) {
        return Fail(L"unexpected end of input");
    }

    const wchar_t ch = text_[position_];
    if (ch == L'{') {
        return ParseObject(out);
    }
    if (ch == L'[') {
        return ParseArray(out);
    }
    if (ch == L'"') {
        std::wstring text;
        if (!ParseString(text)) {
            return false;
        }
        out.type = Value::Type::String;
        out.text = std::move(text);
        return true;
    }
    if (ch == L't') {
        return ParseLiteral(L"true", out, Value::Type::Bool, true);
    }
    if (ch == L'f') {
        return ParseLiteral(L"false", out, Value::Type::Bool, false);
    }
    if (ch == L'n') {
        return ParseLiteral(L"null", out, Value::Type::Null, false);
    }
    if (ch == L'-' || (ch >= L'0' && ch <= L'9')) {
        return ParseNumber(out);
    }
    return Fail(L"unexpected character");
}

bool Parser::ParseObject(Value& out) {
    ++position_; // {
    out.type = Value::Type::Object;
    SkipWhitespace();
    if (Consume(L'}')) {
        return true;
    }

    for (;;) {
        SkipWhitespace();
        std::wstring key;
        if (!ParseString(key)) {
            return false;
        }
        SkipWhitespace();
        if (!Consume(L':')) {
            return Fail(L"expected ':'");
        }

        auto value = std::make_unique<Value>();
        if (!ParseValue(*value)) {
            return false;
        }
        out.object.emplace_back(std::move(key), std::move(value));

        SkipWhitespace();
        if (Consume(L',')) {
            continue;
        }
        if (Consume(L'}')) {
            return true;
        }
        return Fail(L"expected ',' or '}'");
    }
}

bool Parser::ParseArray(Value& out) {
    ++position_; // [
    out.type = Value::Type::Array;
    SkipWhitespace();
    if (Consume(L']')) {
        return true;
    }

    for (;;) {
        auto value = std::make_unique<Value>();
        if (!ParseValue(*value)) {
            return false;
        }
        out.array.push_back(std::move(value));

        SkipWhitespace();
        if (Consume(L',')) {
            continue;
        }
        if (Consume(L']')) {
            return true;
        }
        return Fail(L"expected ',' or ']'");
    }
}

bool Parser::ParseString(std::wstring& out) {
    if (!Consume(L'"')) {
        return Fail(L"expected string");
    }

    while (position_ < text_.size()) {
        const wchar_t ch = text_[position_++];
        if (ch == L'"') {
            return true;
        }
        if (ch != L'\\') {
            out += ch;
            continue;
        }

        if (position_ >= text_.size()) {
            return Fail(L"unterminated escape");
        }
        const wchar_t escape = text_[position_++];
        switch (escape) {
        case L'"': out += L'"'; break;
        case L'\\': out += L'\\'; break;
        case L'/': out += L'/'; break;
        case L'b': out += L'\b'; break;
        case L'f': out += L'\f'; break;
        case L'n': out += L'\n'; break;
        case L'r': out += L'\r'; break;
        case L't': out += L'\t'; break;
        case L'u': {
            unsigned code = 0;
            if (!ParseHex4(code)) {
                return false;
            }
            out += static_cast<wchar_t>(code);
            break;
        }
        default:
            return Fail(L"invalid escape");
        }
    }

    return Fail(L"unterminated string");
}

bool Parser::ParseHex4(unsigned& out) {
    if (position_ + 4 > text_.size()) {
        return Fail(L"short unicode escape");
    }

    out = 0;
    for (int index = 0; index < 4; ++index) {
        const wchar_t ch = text_[position_++];
        out <<= 4;
        if (ch >= L'0' && ch <= L'9') {
            out |= static_cast<unsigned>(ch - L'0');
        } else if (ch >= L'a' && ch <= L'f') {
            out |= static_cast<unsigned>(ch - L'a' + 10);
        } else if (ch >= L'A' && ch <= L'F') {
            out |= static_cast<unsigned>(ch - L'A' + 10);
        } else {
            return Fail(L"invalid unicode escape");
        }
    }
    return true;
}

bool Parser::ParseNumber(Value& out) {
    const size_t start = position_;

    if (text_[position_] == L'-') {
        ++position_;
    }
    while (position_ < text_.size() && iswdigit(text_[position_])) {
        ++position_;
    }
    if (position_ < text_.size() && text_[position_] == L'.') {
        ++position_;
        while (position_ < text_.size() && iswdigit(text_[position_])) {
            ++position_;
        }
    }
    if (position_ < text_.size() && (text_[position_] == L'e' || text_[position_] == L'E')) {
        ++position_;
        if (position_ < text_.size() && (text_[position_] == L'+' || text_[position_] == L'-')) {
            ++position_;
        }
        while (position_ < text_.size() && iswdigit(text_[position_])) {
            ++position_;
        }
    }

    if (position_ == start) {
        return Fail(L"invalid number");
    }

    out.type = Value::Type::Number;
    out.text = text_.substr(start, position_ - start);
    return true;
}

bool Parser::ParseLiteral(const wchar_t* literal, Value& out, Value::Type type, bool boolean) {
    const size_t length = wcslen(literal);
    if (text_.compare(position_, length, literal) != 0) {
        return Fail(L"invalid literal");
    }
    position_ += length;
    out.type = type;
    out.boolean = boolean;
    return true;
}

} // namespace

bool Value::AsBool(bool fallback) const {
    return type == Type::Bool ? boolean : fallback;
}

std::wstring Value::AsString(const std::wstring& fallback) const {
    return type == Type::String ? text : fallback;
}

double Value::AsDouble(double fallback) const {
    if (type != Type::Number) {
        return fallback;
    }
    return wcstod(text.c_str(), nullptr);
}

long long Value::AsInt64(long long fallback) const {
    if (type != Type::Number) {
        return fallback;
    }
    return _wcstoi64(text.c_str(), nullptr, 10);
}

unsigned long long Value::AsUInt64(unsigned long long fallback) const {
    if (type != Type::Number) {
        return fallback;
    }
    return _wcstoui64(text.c_str(), nullptr, 10);
}

const Value* Value::Find(const std::wstring& key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& entry : object) {
        if (entry.first == key) {
            return entry.second.get();
        }
    }
    return nullptr;
}

Value Parse(const std::wstring& text, std::wstring* error) {
    Parser parser(text);
    Value value;
    parser.SkipWhitespace();
    if (!parser.ParseValue(value)) {
        if (error != nullptr) {
            *error = parser.error;
        }
        return {};
    }
    parser.SkipWhitespace();
    if (parser.Position() != text.size()) {
        if (error != nullptr) {
            *error = L"trailing characters at offset " + std::to_wstring(parser.Position());
        }
        return {};
    }
    return value;
}

std::wstring Escape(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        switch (ch) {
        case L'"': result += L"\\\""; break;
        case L'\\': result += L"\\\\"; break;
        case L'\b': result += L"\\b"; break;
        case L'\f': result += L"\\f"; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t buffer[8]{};
                swprintf_s(buffer, L"\\u%04X", static_cast<unsigned>(ch));
                result += buffer;
            } else {
                result += ch;
            }
            break;
        }
    }
    return result;
}

}
