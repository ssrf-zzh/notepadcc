#include "edit/TextTools.h"

#include <windows.h>

#include <algorithm>

namespace npcc::edit {
namespace {

bool IsAsciiSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r' || ch == '\n';
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

// 按代码页把字节解成宽字符，做一次大小写映射，再编回去。
// 系统的大小写表覆盖非 ASCII（拉丁扩展、西里尔、希腊…），中文这类没有大小写的字符原样保留。
std::string MapCase(std::string_view text, std::uint32_t codePage, bool upper) {
    if (text.empty()) {
        return {};
    }
    const UINT page = static_cast<UINT>(codePage == 0 ? CP_UTF8 : codePage);
    const int wideCount = ::MultiByteToWideChar(page, 0, text.data(),
                                                static_cast<int>(text.size()), nullptr, 0);
    if (wideCount <= 0) {
        return std::string(text);   // 解释不了就原样返回，绝不猜
    }
    std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
    ::MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          wideCount);
    if (upper) {
        ::CharUpperBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    } else {
        ::CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    }
    const int bytes = ::WideCharToMultiByte(page, 0, wide.data(), static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string(text);
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(page, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes,
                          nullptr, nullptr);
    return out;
}

bool IsHexDigit(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

unsigned HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<unsigned>(ch - 'a' + 10);
    }
    return static_cast<unsigned>(ch - 'A' + 10);
}

void AppendUtf8(std::string& out, unsigned codePoint) {
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

// 把 "&#123;" / "&#x1F600;" 这种数字实体解成一个码点（转成 UTF-8 写出）。
// 认不出来（位数不对、越界、混进非数字）就返回 false，由调用方照原样保留。
bool AppendHtmlNumber(std::string& out, std::string_view entity) {
    unsigned codePoint = 0;
    if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
        for (std::size_t k = 2; k < entity.size(); ++k) {
            if (!IsHexDigit(entity[k])) {
                return false;
            }
            codePoint = codePoint * 16 + HexValue(entity[k]);
            if (codePoint > 0x10FFFF) {
                return false;
            }
        }
    } else {
        for (std::size_t k = 1; k < entity.size(); ++k) {
            if (entity[k] < '0' || entity[k] > '9') {
                return false;
            }
            codePoint = codePoint * 10 + static_cast<unsigned>(entity[k] - '0');
            if (codePoint > 0x10FFFF) {
                return false;
            }
        }
    }
    if (codePoint == 0) {
        return false;
    }
    AppendUtf8(out, codePoint);
    return true;
}

}  // namespace

LineSplit SplitLines(std::string_view text) {
    LineSplit result;
    // 行尾取"出现次数最多的那一种"：混合行尾的文本按主流风格统一，而不是随便挑一个
    std::size_t crlf = 0;
    std::size_t lf = 0;
    std::size_t cr = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (text[i] == '\n') {
            ++lf;
        }
    }
    result.eol = "\n";
    if (crlf > 0 && crlf >= lf && crlf >= cr) {
        result.eol = "\r\n";
    } else if (cr > lf && cr > 0) {
        result.eol = "\r";
    }

    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size();) {
        const char ch = text[i];
        if (ch != '\r' && ch != '\n') {
            ++i;
            continue;
        }
        result.lines.emplace_back(text.substr(start, i - start));
        if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            i += 2;
        } else {
            ++i;
        }
        start = i;
        result.trailingEol = (i >= text.size());
    }
    if (start < text.size()) {
        result.lines.emplace_back(text.substr(start));
        result.trailingEol = false;
    }
    return result;
}

std::string JoinLines(const std::vector<std::string>& lines, const std::string& eol,
                      bool trailingEol) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || trailingEol) {
            out += eol;
        }
    }
    return out;
}

std::string SortLines(std::string_view text, SortOrder order, bool caseSensitive) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> lines = parts.lines;
    std::stable_sort(lines.begin(), lines.end(), [order, caseSensitive](const std::string& a,
                                                                       const std::string& b) {
        // 忽略大小写时按小写比较；完全相同的行由 stable_sort 保持原有顺序
        const std::string left = caseSensitive ? a : LowerAscii(a);
        const std::string right = caseSensitive ? b : LowerAscii(b);
        return order == SortOrder::Ascending ? left < right : left > right;
    });
    return JoinLines(lines, parts.eol, parts.trailingEol);
}

std::string SortLinesByLength(std::string_view text, SortOrder order) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> lines = parts.lines;
    std::stable_sort(lines.begin(), lines.end(),
                     [order](const std::string& a, const std::string& b) {
                         return order == SortOrder::Ascending ? a.size() < b.size()
                                                              : a.size() > b.size();
                     });
    return JoinLines(lines, parts.eol, parts.trailingEol);
}

std::string RemoveEmptyLines(std::string_view text) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> kept;
    kept.reserve(parts.lines.size());
    for (const std::string& line : parts.lines) {
        if (!line.empty()) {
            kept.push_back(line);
        }
    }
    return JoinLines(kept, parts.eol, parts.trailingEol);
}

std::string RemoveDuplicateLines(std::string_view text, bool keepLast) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> kept;
    if (!keepLast) {
        for (const std::string& line : parts.lines) {
            if (std::find(kept.begin(), kept.end(), line) == kept.end()) {
                kept.push_back(line);
            }
        }
    } else {
        // 保留最后一次出现：位置也按最后一次算，所以从后往前挑再翻回来
        std::vector<std::string> seen;
        for (std::size_t i = parts.lines.size(); i-- > 0;) {
            const std::string& line = parts.lines[i];
            if (std::find(seen.begin(), seen.end(), line) == seen.end()) {
                seen.push_back(line);
                kept.push_back(line);
            }
        }
        std::reverse(kept.begin(), kept.end());
    }
    return JoinLines(kept, parts.eol, parts.trailingEol);
}

std::string ReverseLines(std::string_view text) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> lines = parts.lines;
    std::reverse(lines.begin(), lines.end());
    return JoinLines(lines, parts.eol, parts.trailingEol);
}

std::string TrimTrailingWhitespace(std::string_view text) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> lines;
    lines.reserve(parts.lines.size());
    for (const std::string& line : parts.lines) {
        std::size_t end = line.size();
        while (end > 0 && IsAsciiSpace(line[end - 1])) {
            --end;
        }
        lines.emplace_back(line.substr(0, end));
    }
    return JoinLines(lines, parts.eol, parts.trailingEol);
}

std::string TrimLeadingWhitespace(std::string_view text) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> lines;
    lines.reserve(parts.lines.size());
    for (const std::string& line : parts.lines) {
        std::size_t start = 0;
        while (start < line.size() && IsAsciiSpace(line[start])) {
            ++start;
        }
        lines.emplace_back(line.substr(start));
    }
    return JoinLines(lines, parts.eol, parts.trailingEol);
}

std::string AddLineNumbers(std::string_view text, long long start, long long step, int padWidth) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> lines;
    lines.reserve(parts.lines.size());
    long long number = start;
    for (const std::string& line : parts.lines) {
        std::string digits = std::to_string(number);
        if (padWidth > static_cast<int>(digits.size())) {
            digits.insert(0, static_cast<std::size_t>(padWidth) - digits.size(), ' ');
        }
        lines.push_back(digits + "\t" + line);
        number += step;
    }
    return JoinLines(lines, parts.eol, parts.trailingEol);
}

std::string StripLineNumbers(std::string_view text) {
    const LineSplit parts = SplitLines(text);
    std::vector<std::string> lines;
    lines.reserve(parts.lines.size());
    for (const std::string& line : parts.lines) {
        std::size_t index = 0;
        while (index < line.size() && line[index] == ' ') {
            ++index;
        }
        const std::size_t digitsStart = index;
        while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
            ++index;
        }
        // 必须"有数字"且"后面紧跟一个分隔符"才认，否则这一行原样保留——
        // 免得把正文开头的数字也砍掉
        const bool looksLikeNumber = index > digitsStart && index < line.size() &&
                                     (line[index] == '\t' || line[index] == ' ' ||
                                      line[index] == '.' || line[index] == ':');
        if (looksLikeNumber) {
            std::size_t body = index + 1;
            while (body < line.size() && line[body] == ' ') {
                ++body;
            }
            lines.emplace_back(line.substr(body));
        } else {
            lines.push_back(line);
        }
    }
    return JoinLines(lines, parts.eol, parts.trailingEol);
}

std::string ToUpperCase(std::string_view text, std::uint32_t codePage) {
    return MapCase(text, codePage, true);
}

std::string ToLowerCase(std::string_view text, std::uint32_t codePage) {
    return MapCase(text, codePage, false);
}

std::string Base64Encode(std::string_view bytes) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const unsigned triple = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                                static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(alphabet[(triple >> 6) & 0x3F]);
        out.push_back(alphabet[triple & 0x3F]);
        i += 3;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const unsigned value = static_cast<unsigned char>(bytes[i]) << 16;
        out.push_back(alphabet[(value >> 18) & 0x3F]);
        out.push_back(alphabet[(value >> 12) & 0x3F]);
        out += "==";
    } else if (rest == 2) {
        const unsigned value = (static_cast<unsigned char>(bytes[i]) << 16) |
                               (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(value >> 18) & 0x3F]);
        out.push_back(alphabet[(value >> 12) & 0x3F]);
        out.push_back(alphabet[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool Base64Decode(std::string_view text, std::string& out) {
    auto valueOf = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    std::string filtered;
    filtered.reserve(text.size());
    for (const char ch : text) {
        if (IsAsciiSpace(ch)) {
            continue;   // 换行与空格只是排版，不算内容
        }
        filtered.push_back(ch);
    }
    if (filtered.size() % 4 != 0) {
        return false;   // 长度不是 4 的倍数：多半是把残缺的内容粘进来了
    }
    std::string decoded;
    decoded.reserve(filtered.size() / 4 * 3);
    for (std::size_t i = 0; i < filtered.size(); i += 4) {
        int values[4] = {0, 0, 0, 0};
        int padding = 0;
        for (int k = 0; k < 4; ++k) {
            const char ch = filtered[i + static_cast<std::size_t>(k)];
            if (ch == '=') {
                if (k < 2 || i + 4 != filtered.size()) {
                    return false;   // 填充只能出现在末尾，且最多两个
                }
                ++padding;
                continue;
            }
            if (padding > 0) {
                return false;       // 填充之后又出现内容
            }
            values[k] = valueOf(ch);
            if (values[k] < 0) {
                return false;
            }
        }
        const unsigned triple = (static_cast<unsigned>(values[0]) << 18) |
                                (static_cast<unsigned>(values[1]) << 12) |
                                (static_cast<unsigned>(values[2]) << 6) |
                                static_cast<unsigned>(values[3]);
        decoded.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (padding < 2) {
            decoded.push_back(static_cast<char>((triple >> 8) & 0xFF));
        }
        if (padding < 1) {
            decoded.push_back(static_cast<char>(triple & 0xFF));
        }
    }
    out = std::move(decoded);
    return true;
}

std::string UrlEncode(std::string_view text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        if (unreserved) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[(byte >> 4) & 0xF]);
            out.push_back(hex[byte & 0xF]);
        }
    }
    return out;
}

bool UrlDecode(std::string_view text, std::string& out) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            decoded.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size() || !IsHexDigit(text[i + 1]) || !IsHexDigit(text[i + 2])) {
            return false;   // % 后面不是两位十六进制：宁可报错，也不要猜
        }
        decoded.push_back(static_cast<char>((HexValue(text[i + 1]) << 4) | HexValue(text[i + 2])));
        i += 2;
    }
    out = std::move(decoded);
    return true;
}

std::string EscapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out.push_back(ch); break;
        }
    }
    return out;
}

std::string UnescapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i++]);
            continue;
        }
        const std::size_t end = text.find(';', i);
        if (end == std::string_view::npos) {
            out.push_back(text[i++]);   // 没有分号就不当实体处理
            continue;
        }
        const std::string_view entity = text.substr(i + 1, end - i - 1);
        bool handled = true;
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos" || entity == "#39") {
            out.push_back('\'');
        } else if (entity.size() > 1 && entity[0] == '#') {
            handled = AppendHtmlNumber(out, entity);
        } else {
            handled = false;
        }
        if (handled) {
            i = end + 1;
        } else {
            out.push_back(text[i++]);   // 不认识的实体原样保留
        }
    }
    return out;
}

}  // namespace npcc::edit
