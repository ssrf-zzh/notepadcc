#include "core/ExportHtml.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "edit/Encoding.h"

namespace npcc::core {
namespace {

std::string Hex(std::uint32_t rgb) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "#%06X", rgb & 0xFFFFFFu);
    return buffer;
}

// HTML 文本转义：`&` 必须第一个换，否则后面换出来的 `&lt;` 会被再转一次
std::string EscapeInto(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

}  // namespace

std::vector<StyledRun> GroupStyledBytes(const std::vector<StyledByte>& bytes) {
    std::vector<StyledRun> runs;
    for (const StyledByte& byte : bytes) {
        if (!runs.empty() && runs.back().style == byte.style) {
            runs.back().text.push_back(byte.ch);
            continue;
        }
        StyledRun run;
        run.style = byte.style;
        run.text.push_back(byte.ch);
        runs.push_back(std::move(run));
    }
    return runs;
}

std::string EscapeHtmlText(std::string_view text) {
    return EscapeInto(text);
}

std::vector<StyledByte> StyledBytesAsUtf8(const std::vector<StyledByte>& bytes,
                                          std::uint32_t codePage) {
    if (bytes.empty() || codePage == edit::kCodePageUtf8) {
        return bytes;   // UTF-8 的文档直通：绝大多数文件走的都是这条路
    }
    std::vector<StyledByte> out;
    out.reserve(bytes.size());
    std::size_t index = 0;
    while (index < bytes.size()) {
        const unsigned char lead = static_cast<unsigned char>(bytes[index].ch);
        std::size_t length = 1;
        // 双字节代码页（GBK / Big5 / Shift-JIS…）的**前导字节要连着后一个字节一起解**，
        // 否则一个汉字会被拆成两个看不懂的字符。用系统给的那张前导字节表，别自己写范围
        if (lead >= 0x80 && ::IsDBCSLeadByteEx(codePage, lead) != 0 &&
            index + 1 < bytes.size()) {
            length = 2;
        }
        // 注意：**不能**把 &bytes[index].ch 当字节序列用。StyledByte 是
        // {char ch; int style;}，ch 与 style 之间有对齐填充，"紧跟其后的那个字节"是填充
        // 而不是下一个字符的字节（踩过：GBK 的双字节汉字在这里永远解不出来）。
        // 老老实实把要解的这几个字节抄进一个小数组
        char source[2] = {bytes[index].ch, 0};
        if (length == 2) {
            source[1] = bytes[index + 1].ch;
        }
        wchar_t wide = 0;
        const int decoded = ::MultiByteToWideChar(codePage, 0, source,
                                                  static_cast<int>(length), &wide, 1);
        if (decoded != 1) {
            // 解不出来（截断的前导字节、非法序列）：原样吐出去，至少不丢内容
            out.push_back(bytes[index]);
            ++index;
            continue;
        }
        char utf8[8] = {};
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, &wide, 1, utf8,
                                                  static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        for (int i = 0; i < written; ++i) {
            out.push_back(StyledByte{utf8[i], bytes[index].style});   // 样式跟着字符走
        }
        index += length;
    }
    return out;
}

std::string BuildHtml(const std::vector<StyledByte>& bytes,
                      const std::vector<ExportStyle>& styles, const ExportOptions& options) {
    // 用到哪些样式号（只给用到的写 CSS，导出的文件才干净）
    std::set<int> used;
    for (const StyledByte& byte : bytes) {
        used.insert(byte.style);
    }
    std::map<int, ExportStyle> byNumber;
    for (const ExportStyle& style : styles) {
        byNumber[style.number] = style;
    }

    std::string html;
    html += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>";
    html += EscapeInto(options.title);
    html += "</title>\n<style>\n";
    html += "body { margin: 0; background: " + Hex(options.pageBack) + "; color: " +
            Hex(options.pageFore) + "; }\n";
    html += "table.code { border-collapse: collapse; font-family: '" + options.fontFace +
            "', monospace; font-size: " + std::to_string(options.fontSize) + "pt; }\n";
    if (options.lineNumbers) {
        html += "td.ln { color: #808080; text-align: right; padding-right: 1em; "
                "user-select: none; vertical-align: top; }\n";
    }
    html += "td.code { white-space: pre; }\n";
    for (const int number : used) {
        const auto found = byNumber.find(number);
        if (found == byNumber.end()) {
            continue;   // 没有外观信息的样式号：用页面默认色，不硬编一个
        }
        const ExportStyle& style = found->second;
        if (style.fore == options.pageFore && !style.bold && !style.italic) {
            continue;   // 与默认色完全相同、又没有粗斜体：写出来只是噪音
        }
        html += "span.s" + std::to_string(number) + " { color: " + Hex(style.fore) + ";";
        if (style.bold) {
            html += " font-weight: bold;";
        }
        if (style.italic) {
            html += " font-style: italic;";
        }
        html += " }\n";
    }
    html += "</style>\n</head>\n<body>\n<table class=\"code\">\n";

    // 先按行切开（CRLF 与 CR 都当换行、不留在行里），再逐行把同一样式合并成一段。
    // 先切行再合并，比"边扫边拼"清楚得多——行号与代码列必须严格对齐
    std::vector<std::vector<StyledByte>> lines;
    lines.emplace_back();
    for (const StyledByte& byte : bytes) {
        if (byte.ch == '\r') {
            continue;
        }
        if (byte.ch == '\n') {
            lines.emplace_back();
            continue;
        }
        lines.back().push_back(byte);
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
        html += "<tr>";
        if (options.lineNumbers) {
            html += "<td class=\"ln\">" + std::to_string(index + 1) + "</td>";
        }
        html += "<td class=\"code\">";
        for (const StyledRun& run : GroupStyledBytes(lines[index])) {
            if (run.style == 0) {
                html += EscapeInto(run.text);
            } else {
                html += "<span class=\"s" + std::to_string(run.style) + "\">" +
                        EscapeInto(run.text) + "</span>";
            }
        }
        html += "</td></tr>\n";
    }
    html += "</table>\n</body>\n</html>\n";
    return html;
}

}  // namespace npcc::core
