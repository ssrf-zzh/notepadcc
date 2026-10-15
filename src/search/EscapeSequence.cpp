#include "search/EscapeSequence.h"

#include <cstdint>

namespace npcc::search {
namespace {

bool HexValue(char ch, unsigned int& value) {
    if (ch >= '0' && ch <= '9') {
        value = static_cast<unsigned int>(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        value = static_cast<unsigned int>(ch - 'a') + 10u;
        return true;
    }
    if (ch >= 'A' && ch <= 'F') {
        value = static_cast<unsigned int>(ch - 'A') + 10u;
        return true;
    }
    return false;
}

void AppendUtf8(std::string& out, unsigned long codePoint) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

}  // namespace

bool UnescapeExtended(std::string_view pattern, std::string& out, SearchOutcome& outcome) {
    out.clear();
    out.reserve(pattern.size());

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i + 1 >= pattern.size()) {
            out.push_back('\\');   // 结尾的孤立反斜杠按字面量处理
            break;
        }
        const char next = pattern[++i];
        switch (next) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '0': out.push_back('\0'); break;
            case 'a': out.push_back('\a'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '\\': out.push_back('\\'); break;
            case 'x':
            case 'u': {
                unsigned long codePoint = 0;
                bool parsed = false;
                if (i + 1 < pattern.size() && pattern[i + 1] == '{') {
                    // \x{NNNN}
                    const std::size_t close = pattern.find('}', i + 2);
                    if (close == std::string_view::npos) {
                        outcome = SearchOutcome::Failure(
                            SearchError::BadPattern, L"\\x{ 缺少收尾的 }");
                        return false;
                    }
                    for (std::size_t k = i + 2; k < close; ++k) {
                        unsigned int value = 0;
                        if (!HexValue(pattern[k], value)) {
                            outcome = SearchOutcome::Failure(
                                SearchError::BadPattern, L"\\x{} 中出现了非十六进制字符");
                            return false;
                        }
                        codePoint = codePoint * 16 + value;
                    }
                    i = close;
                    parsed = true;
                } else {
                    // \xNN：固定两位十六进制
                    for (std::size_t k = 0; k < 2; ++k) {
                        // 每次都看紧邻的下一个字符；i 会随消费前移，
                        // 若写成 i + 1 + k 就会越读越远（曾经的第一版就是错的）。
                        if (i + 1 >= pattern.size()) {
                            break;
                        }
                        unsigned int value = 0;
                        if (!HexValue(pattern[i + 1], value)) {
                            break;
                        }
                        codePoint = codePoint * 16 + value;
                        ++i;
                        parsed = true;
                    }
                }
                if (!parsed) {
                    // 不认识的写法原样保留，别把用户输入吃掉
                    out.push_back('\\');
                    out.push_back(next);
                    break;
                }
                if (codePoint > 0x10FFFF) {
                    outcome = SearchOutcome::Failure(
                        SearchError::BadPattern, L"\\x 码点超出 Unicode 范围");
                    return false;
                }
                if (codePoint < 0x80) {
                    out.push_back(static_cast<char>(codePoint));
                } else {
                    AppendUtf8(out, codePoint);
                }
                break;
            }
            default:
                out.push_back('\\');
                out.push_back(next);
                break;
        }
    }
    return true;
}

}  // namespace npcc::search
