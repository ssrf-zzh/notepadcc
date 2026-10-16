#include "core/JsonFormat.h"

#include <cctype>
#include <vector>

namespace npcc::core {
namespace {

// 期望什么：一个值（键或数组元素）、一个键、冒号、逗号或收尾
enum class Expect { Value, KeyOrClose, Key, Colon, CommaOrClose, Done };

class Formatter {
public:
    Formatter(std::string_view text, int indent, bool minify)
        : text_(text), indent_(std::max(0, std::min(16, indent))), minify_(minify) {}

    FormatResult Run() {
        SkipSpace();
        if (AtEnd()) {
            return FormatResult::Fail(L"内容为空，没有可格式化的 JSON");
        }
        if (!ParseValue()) {
            return FormatResult::Fail(error_);
        }
        SkipSpace();
        if (!AtEnd()) {
            return FormatResult::Fail(Position() + L"顶层值之后还有内容");
        }
        if (!stack_.empty()) {
            return FormatResult::Fail(L"括号没有闭合");
        }
        out_.push_back('\n');
        return FormatResult::Ok(std::move(out_));
    }

private:
    bool AtEnd() const { return index_ >= text_.size(); }
    char Peek() const { return AtEnd() ? '\0' : text_[index_]; }

    std::wstring Position() const {
        int line = 1;
        int column = 1;
        for (std::size_t i = 0; i < index_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return L"第 " + std::to_wstring(line) + L" 行第 " + std::to_wstring(column) + L" 列：";
    }

    void SkipSpace() {
        while (!AtEnd()) {
            const char ch = text_[index_];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                ++index_;
                continue;
            }
            break;
        }
    }

    void Indent(std::size_t depth) {
        if (minify_) {
            return;
        }
        out_.push_back('\n');
        out_.append(depth * static_cast<std::size_t>(indent_), ' ');
    }

    // 值：对象、数组、字符串、数字、true/false/null
    bool ParseValue() {
        SkipSpace();
        const char ch = Peek();
        if (ch == '{') {
            return ParseObject();
        }
        if (ch == '[') {
            return ParseArray();
        }
        if (ch == '"') {
            return CopyString();
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            return CopyNumber();
        }
        if (Match("true") || Match("false") || Match("null")) {
            return true;
        }
        if (AtEnd()) {
            error_ = Position() + L"内容在这里就结束了，缺少一个值";
            return false;
        }
        error_ = Position() + L"这里应当是一个值，实际是 '" + std::wstring(1, static_cast<wchar_t>(
                     static_cast<unsigned char>(ch))) + L"'";
        return false;
    }

    bool ParseObject() {
        ++index_;   // '{'
        stack_.push_back('{');
        SkipSpace();
        if (Peek() == '}') {
            ++index_;
            stack_.pop_back();
            out_ += "{}";
            return true;
        }
        out_.push_back('{');
        for (;;) {
            Indent(stack_.size());
            if (Peek() != '"') {
                error_ = Position() + L"对象的键必须是字符串";
                return false;
            }
            std::size_t keyStart = index_;
            if (!CopyString()) {
                return false;
            }
            (void)keyStart;
            SkipSpace();
            if (Peek() != ':') {
                error_ = Position() + L"键之后应当是冒号";
                return false;
            }
            ++index_;
            out_ += minify_ ? ":" : ": ";
            if (!ParseValue()) {
                return false;
            }
            SkipSpace();
            if (Peek() == ',') {
                ++index_;
                out_.push_back(',');
                SkipSpace();
                if (Peek() == '}') {
                    error_ = Position() + L"逗号后面直接跟了右花括号（JSON 不允许尾随逗号）";
                    return false;
                }
                continue;
            }
            if (Peek() == '}') {
                ++index_;
                stack_.pop_back();
                Indent(stack_.size());
                out_.push_back('}');
                return true;
            }
            error_ = Position() + L"这里应当是逗号或右花括号";
            return false;
        }
    }

    bool ParseArray() {
        ++index_;   // '['
        stack_.push_back('[');
        SkipSpace();
        if (Peek() == ']') {
            ++index_;
            stack_.pop_back();
            out_ += "[]";
            return true;
        }
        out_.push_back('[');
        for (;;) {
            Indent(stack_.size());
            if (!ParseValue()) {
                return false;
            }
            SkipSpace();
            if (Peek() == ',') {
                ++index_;
                out_.push_back(',');
                SkipSpace();
                if (Peek() == ']') {
                    error_ = Position() + L"逗号后面直接跟了右方括号（JSON 不允许尾随逗号）";
                    return false;
                }
                continue;
            }
            if (Peek() == ']') {
                ++index_;
                stack_.pop_back();
                Indent(stack_.size());
                out_.push_back(']');
                return true;
            }
            error_ = Position() + L"这里应当是逗号或右方括号";
            return false;
        }
    }

    // 字符串**原样搬运**（含转义写法），只做合法性校验
    bool CopyString() {
        const std::size_t begin = index_;
        ++index_;   // 开引号
        while (!AtEnd()) {
            const char ch = text_[index_++];
            if (ch == '"') {
                out_.append(text_.substr(begin, index_ - begin));
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                --index_;
                error_ = Position() + L"字符串里有未转义的控制字符";
                return false;
            }
            if (ch != '\\') {
                continue;
            }
            if (AtEnd()) {
                break;
            }
            const char escaped = text_[index_++];
            switch (escaped) {
                case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r':
                case 't':
                    break;
                case 'u': {
                    // \uXXXX：四个十六进制数字，少一个都不认
                    for (int digit = 0; digit < 4; ++digit) {
                        if (AtEnd() || std::isxdigit(static_cast<unsigned char>(text_[index_])) == 0) {
                            error_ = Position() + L"\\u 后面必须跟四个十六进制数字";
                            return false;
                        }
                        ++index_;
                    }
                    break;
                }
                default:
                    error_ = Position() + L"字符串里有非法的转义 \\" +
                             std::wstring(1, static_cast<wchar_t>(
                                                 static_cast<unsigned char>(escaped)));
                    return false;
            }
        }
        error_ = L"字符串没有收尾的引号";
        return false;
    }

    // 数字同样原样搬运（1.500 不该被写成 1.5），只校验形态
    bool CopyNumber() {
        const std::size_t begin = index_;
        if (Peek() == '-') {
            ++index_;
        }
        if (AtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
            error_ = Position() + L"减号后面应当是数字";
            return false;
        }
        if (Peek() == '0') {
            ++index_;   // JSON 不允许前导零
        } else {
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++index_;
            }
        }
        if (Peek() == '.') {
            ++index_;
            if (AtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                error_ = Position() + L"小数点后面应当是数字";
                return false;
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++index_;
            }
        }
        if (Peek() == 'e' || Peek() == 'E') {
            ++index_;
            if (Peek() == '+' || Peek() == '-') {
                ++index_;
            }
            if (AtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                error_ = Position() + L"指数部分应当是数字";
                return false;
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++index_;
            }
        }
        out_.append(text_.substr(begin, index_ - begin));
        return true;
    }

    bool Match(std::string_view word) {
        if (text_.compare(index_, word.size(), word) != 0) {
            return false;
        }
        index_ += word.size();
        out_.append(word);
        return true;
    }

    std::string_view       text_;
    std::size_t            index_ = 0;
    int                    indent_ = 2;
    bool                   minify_ = false;
    std::string            out_;
    std::vector<char>      stack_;
    std::wstring           error_;
};

}  // namespace

FormatResult FormatJson(std::string_view text, int indent, bool minify) {
    Formatter formatter(text, indent, minify);
    return formatter.Run();
}

}  // namespace npcc::core
