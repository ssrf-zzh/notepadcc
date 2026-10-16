#include "core/JsonFlat.h"

#include <cstdlib>

namespace npcc::core {
namespace {

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    bool Parse(JsonObject& out, std::wstring* error) {
        SkipSpace();
        if (!Expect('{')) {
            return Fail(error, L"对象要以 { 开头");
        }
        SkipSpace();
        if (Peek() == '}') {
            ++index_;
            SkipSpace();
            return index_ >= text_.size() ? true : Fail(error, L"对象之后还有多余内容");
        }
        for (;;) {
            SkipSpace();
            std::string key;
            if (!ParseString(key)) {
                return Fail(error, L"键名不是合法的字符串");
            }
            SkipSpace();
            if (!Expect(':')) {
                return Fail(error, L"键名之后应当是冒号");
            }
            SkipSpace();
            JsonValue value;
            if (!ParseValue(value)) {
                return Fail(error, L"值只能是字符串、数字或 true/false");
            }
            out.values[key] = std::move(value);
            SkipSpace();
            const char ch = Peek();
            if (ch == ',') {
                ++index_;
                continue;
            }
            if (ch == '}') {
                ++index_;
                break;
            }
            return Fail(error, L"值之后应当是逗号或右花括号");
        }
        SkipSpace();
        if (index_ < text_.size()) {
            return Fail(error, L"对象之后还有多余内容");
        }
        return true;
    }

private:
    bool Fail(std::wstring* error, const wchar_t* what) {
        if (error != nullptr) {
            *error = std::wstring(what) + L"（第 " + std::to_wstring(index_ + 1) + L" 个字节附近）";
        }
        return false;
    }

    char Peek() const { return index_ < text_.size() ? text_[index_] : '\0'; }

    void SkipSpace() {
        while (index_ < text_.size()) {
            const char ch = text_[index_];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                ++index_;
                continue;
            }
            break;
        }
    }

    bool Expect(char expected) {
        if (Peek() != expected) {
            return false;
        }
        ++index_;
        return true;
    }

    bool ParseString(std::string& out) {
        if (!Expect('"')) {
            return false;
        }
        out.clear();
        while (index_ < text_.size()) {
            const char ch = text_[index_++];
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (index_ >= text_.size()) {
                return false;
            }
            const char escaped = text_[index_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default:
                    // \uXXXX 明确不支持：宁可失败也不要猜
                    return false;
            }
        }
        return false;   // 字符串没有收尾
    }

    bool ParseValue(JsonValue& out) {
        const char ch = Peek();
        if (ch == '"') {
            out.kind = JsonValue::Kind::String;
            return ParseString(out.text);
        }
        if (ch == '[') {
            // 字符串数组（含空数组）：拼成逗号分隔的字符串。plugin.json 的 dependencies
            // 就是这么写的，调用方拿 SplitList 一切就行
            ++index_;
            std::string joined;
            SkipSpace();
            if (Peek() == ']') {
                ++index_;
                out.kind = JsonValue::Kind::String;
                out.text.clear();
                return true;
            }
            for (;;) {
                SkipSpace();
                std::string item;
                if (!ParseString(item)) {
                    return false;
                }
                if (!joined.empty()) {
                    joined.push_back(',');
                }
                joined += item;
                SkipSpace();
                if (Peek() == ',') {
                    ++index_;
                    continue;
                }
                if (Peek() == ']') {
                    ++index_;
                    break;
                }
                return false;
            }
            out.kind = JsonValue::Kind::String;
            out.text = joined;
            return true;
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            const std::size_t begin = index_;
            while (index_ < text_.size()) {
                const char digit = text_[index_];
                if ((digit >= '0' && digit <= '9') || digit == '-' || digit == '+' ||
                    digit == '.' || digit == 'e' || digit == 'E') {
                    ++index_;
                    continue;
                }
                break;
            }
            const std::string token(text_.substr(begin, index_ - begin));
            char* end = nullptr;
            const double value = std::strtod(token.c_str(), &end);
            if (end == token.c_str() || *end != '\0') {
                return false;
            }
            out.kind = JsonValue::Kind::Number;
            out.number = value;
            return true;
        }
        if (Match("true")) {
            out.kind = JsonValue::Kind::Boolean;
            out.boolean = true;
            return true;
        }
        if (Match("false")) {
            out.kind = JsonValue::Kind::Boolean;
            out.boolean = false;
            return true;
        }
        return false;
    }

    bool Match(std::string_view word) {
        if (text_.compare(index_, word.size(), word) != 0) {
            return false;
        }
        index_ += word.size();
        return true;
    }

    std::string_view text_;
    std::size_t      index_ = 0;
};

}  // namespace

bool JsonObject::Has(std::string_view key) const {
    return values.find(std::string(key)) != values.end();
}

std::string JsonObject::StringOr(std::string_view key, std::string_view fallback) const {
    const auto found = values.find(std::string(key));
    if (found == values.end() || found->second.kind != JsonValue::Kind::String) {
        return std::string(fallback);
    }
    return found->second.text;
}

double JsonObject::NumberOr(std::string_view key, double fallback) const {
    const auto found = values.find(std::string(key));
    if (found == values.end() || found->second.kind != JsonValue::Kind::Number) {
        return fallback;
    }
    return found->second.number;
}

bool JsonObject::BoolOr(std::string_view key, bool fallback) const {
    const auto found = values.find(std::string(key));
    if (found == values.end() || found->second.kind != JsonValue::Kind::Boolean) {
        return fallback;
    }
    return found->second.boolean;
}

bool ParseJsonObject(std::string_view utf8, JsonObject& out, std::wstring* error) {
    JsonObject parsed;
    Reader reader(utf8);
    if (!reader.Parse(parsed, error)) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

}  // namespace npcc::core
