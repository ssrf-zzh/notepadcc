#include "core/ExportRtf.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <set>

namespace npcc::core {
namespace {

// RTF 的颜色表是 0..N 的索引，正文用 \cfN 引用。0 号留给"默认前景"。
struct ColourTable {
    std::vector<std::uint32_t> colours;   // 索引即 RTF 的颜色号
    std::map<std::uint32_t, int> index;

    int Add(std::uint32_t rgb) {
        const auto found = index.find(rgb);
        if (found != index.end()) {
            return found->second;
        }
        colours.push_back(rgb);
        const int number = static_cast<int>(colours.size());
        index[rgb] = number;
        return number;
    }
};

// RTF 正文里的转义：\ { } 要加反斜杠；非 ASCII 用 \uNNNN? 形式
// （RTF 的 \uNNNN 是**有符号** 16 位，超过 0x7FFF 的要写成负数）
std::string EscapeRtf(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    std::wstring wide;
    if (need > 0) {
        wide.resize(static_cast<std::size_t>(need));
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                              need);
    } else {
        // 不是合法 UTF-8：按字节原样处理，至少内容不丢
        for (const char ch : utf8) {
            wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
        }
    }
    std::string out;
    for (const wchar_t ch : wide) {
        switch (ch) {
            case L'\\': out += "\\\\"; break;
            case L'{': out += "\\{"; break;
            case L'}': out += "\\}"; break;
            case L'\r': break;   // 行尾统一成 \par，不留 CR
            case L'\n': out += "\\par\n"; break;
            default:
                if (ch >= 0x20 && ch < 0x7F) {
                    out.push_back(static_cast<char>(ch));
                } else {
                    // \uN? ：N 是有符号 16 位；后面的 '?' 是给不认识 Unicode 的阅读器兜底
                    const int value = ch >= 0x8000 ? static_cast<int>(ch) - 0x10000
                                                    : static_cast<int>(ch);
                    char buffer[24] = {};
                    std::snprintf(buffer, sizeof(buffer), "\\u%d?", value);
                    out += buffer;
                }
                break;
        }
    }
    return out;
}

}  // namespace

std::string BuildRtf(const std::vector<StyledByte>& bytes,
                     const std::vector<ExportStyle>& styles, const ExportOptions& options) {
    std::map<int, ExportStyle> byNumber;
    for (const ExportStyle& style : styles) {
        byNumber[style.number] = style;
    }
    ColourTable colours;
    const int defaultColour = colours.Add(options.pageFore);   // 0 号 = 默认前景

    // 颜色表与字体表要先写出来（RTF 的规矩：它们必须出现在正文之前）
    std::string header = "{\\rtf1\\ansi\\ansicpg1252\\deff0\n";
    header += "{\\fonttbl{\\f0\\fmodern " + options.fontFace + ";}}\n";

    std::string body;
    body += "\\viewkind4\\uc1\\f0\\fs" + std::to_string(options.fontSize * 2) + "\n";  // RTF 字号是半磅
    body += "\\cf" + std::to_string(defaultColour);   // 明写一次：别指望阅读器的"自动色"
    int currentStyle = -1;
    int currentColour = defaultColour;
    bool bold = false;
    bool italic = false;
    // 按**样式段**输出：多字节字符必须整段交给转义函数，逐字节切会把 UTF-8 切碎
    for (const StyledRun& run : GroupStyledBytes(bytes)) {
        if (run.style != currentStyle) {
            currentStyle = run.style;
            ExportStyle style;
            style.number = run.style;
            style.fore = options.pageFore;
            const auto found = byNumber.find(run.style);
            if (found != byNumber.end()) {
                style = found->second;
            }
            const int colour = colours.Add(style.fore);
            if (colour != currentColour) {
                body += "\\cf" + std::to_string(colour);
                currentColour = colour;
            }
            if (style.bold != bold) {
                body += style.bold ? "\\b" : "\\b0";
                bold = style.bold;
            }
            if (style.italic != italic) {
                body += style.italic ? "\\i" : "\\i0";
                italic = style.italic;
            }
        }
        body += EscapeRtf(run.text);
    }
    if (bold) {
        body += "\\b0";
    }
    if (italic) {
        body += "\\i0";
    }

    std::string table = "{\\colortbl ;";
    // 0 号已经在表里（分号前那个空位是 RTF 约定的"自动色"），所以从 1 号开始补齐
    std::string colourEntries;
    for (std::size_t index = 1; index < colours.colours.size(); ++index) {
        const std::uint32_t rgb = colours.colours[index];
        colourEntries += "\\red" + std::to_string((rgb >> 16) & 0xFF) + "\\green" +
                         std::to_string((rgb >> 8) & 0xFF) + "\\blue" +
                         std::to_string(rgb & 0xFF) + ";";
    }
    table += colourEntries + "}\n";
    return header + table + body + "}\n";
}

}  // namespace npcc::core
