#include "core/ExportHtml.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

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
