#include "core/MarkdownRender.h"

#include <algorithm>
#include <cstdio>

#include "core/ExportHtml.h"   // EscapeHtmlText

namespace npcc::core {
namespace {

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

bool IsBlank(std::string_view line) {
    for (const char ch : line) {
        if (ch != ' ' && ch != '\t') {
            return false;
        }
    }
    return true;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// 一行是不是"围栏代码块"的开头／结尾：``` 或 ~~~（三个以上），后面可以跟语言名
bool FenceMarker(std::string_view line, std::string* language) {
    std::string_view body = Trim(line);
    if (body.size() < 3) {
        return false;
    }
    const char mark = body.front();
    if (mark != '`' && mark != '~') {
        return false;
    }
    std::size_t count = 0;
    while (count < body.size() && body[count] == mark) {
        ++count;
    }
    if (count < 3) {
        return false;
    }
    if (language != nullptr) {
        *language = std::string(Trim(body.substr(count)));
    }
    return true;
}

// 标题：1~6 个 # 后面跟空白（或直接结束）。要求空白是刻意的——"#标签"不是标题，
// 放宽了会把正文里的 # 也吃成标题
int HeadingLevel(std::string_view line, std::string_view* body) {
    std::size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#' && hashes < 6) {
        ++hashes;
    }
    if (hashes == 0) {
        return 0;
    }
    if (hashes < line.size() && line[hashes] != ' ' && line[hashes] != '\t') {
        return 0;
    }
    *body = Trim(line.substr(hashes));
    // 结尾的一串 # 是装饰（"## 标题 ##"），去掉
    while (!body->empty() && body->back() == '#') {
        body->remove_suffix(1);
    }
    *body = Trim(*body);
    return static_cast<int>(hashes);
}

// 分隔线：一整行只有 3 个以上的同一种符号（- * _），中间可以有空格
bool IsRule(std::string_view line) {
    std::string_view body = Trim(line);
    if (body.size() < 3) {
        return false;
    }
    const char mark = body.front();
    if (mark != '-' && mark != '*' && mark != '_') {
        return false;
    }
    int count = 0;
    for (const char ch : body) {
        if (ch == mark) {
            ++count;
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            continue;
        }
        return false;   // 混进了别的字符：不是分隔线
    }
    return count >= 3;
}

// 列表项：前导空白 + （- * + 或 数字.) + 空白 + 内容。缩进层级按两个空格算一级
bool ListItem(std::string_view line, bool* ordered, int* number, int* level,
              std::string_view* body) {
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') {
        ++indent;
    }
    if (indent >= line.size()) {
        return false;
    }
    const char first = line[indent];
    std::size_t cursor = indent;
    int itemNumber = 0;
    bool isOrdered = false;
    if (first == '-' || first == '*' || first == '+') {
        ++cursor;
    } else if (first >= '0' && first <= '9') {
        while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') {
            itemNumber = itemNumber * 10 + (line[cursor] - '0');
            ++cursor;
        }
        if (cursor >= line.size() || (line[cursor] != '.' && line[cursor] != ')')) {
            return false;
        }
        ++cursor;
        isOrdered = true;
    } else {
        return false;
    }
    if (cursor >= line.size() || (line[cursor] != ' ' && line[cursor] != '\t')) {
        return false;   // "-abc" 不是列表项（CommonMark 也这么要求）
    }
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) {
        ++cursor;
    }
    *ordered = isOrdered;
    *number = isOrdered ? itemNumber : 0;
    *level = static_cast<int>(indent / 2);
    *body = line.substr(cursor);
    return true;
}

void AppendTextSpan(std::vector<MarkdownSpan>& spans, std::string text) {
    if (text.empty()) {
        return;
    }
    if (!spans.empty() && spans.back().kind == MarkdownSpanKind::Text) {
        spans.back().text += text;   // 相邻的普通文本合成一段，免得碎片化
        return;
    }
    MarkdownSpan span;
    span.kind = MarkdownSpanKind::Text;
    span.text = std::move(text);
    spans.push_back(std::move(span));
}

void AppendSpan(std::vector<MarkdownSpan>& spans, MarkdownSpanKind kind, std::string text) {
    MarkdownSpan span;
    span.kind = kind;
    span.text = std::move(text);
    spans.push_back(std::move(span));
}

// 找成对的定界符（**bold** 这一类）。返回内容区间；找不到返回 false
bool FindDelimited(std::string_view line, std::size_t start, std::string_view delimiter,
                   std::size_t* contentEnd) {
    const std::size_t close = line.find(delimiter, start);
    if (close == std::string_view::npos || close == start) {
        return false;   // 没有收尾，或者空的（****）——都不当强调处理
    }
    *contentEnd = close;
    return true;
}

std::string ListPrefixText(bool ordered, int number) {
    if (ordered) {
        char buffer[24] = {};
        std::snprintf(buffer, sizeof(buffer), "%d. ", number);
        return buffer;
    }
    return "\xE2\x80\xA2 ";   // "• "（UTF-8）
}

}  // namespace

std::vector<MarkdownSpan> ParseInline(std::string_view line) {
    std::vector<MarkdownSpan> spans;
    std::string text;
    std::size_t index = 0;
    while (index < line.size()) {
        const char ch = line[index];
        if (ch == '\\' && index + 1 < line.size()) {
            text.push_back(line[index + 1]);   // 转义：\* 就是一个星号
            index += 2;
            continue;
        }
        if (ch == '`') {
            const std::size_t close = line.find('`', index + 1);
            if (close != std::string_view::npos && close > index + 1) {
                AppendTextSpan(spans, text);
                text.clear();
                AppendSpan(spans, MarkdownSpanKind::Code,
                           std::string(line.substr(index + 1, close - index - 1)));
                index = close + 1;
                continue;
            }
        }
        if (ch == '*' || ch == '_') {
            const bool doubled = index + 1 < line.size() && line[index + 1] == ch;
            const std::string_view delimiter = doubled ? (ch == '*' ? std::string_view("**")
                                                                   : std::string_view("__"))
                                                       : std::string_view(&line[index], 1);
            std::size_t contentEnd = 0;
            if (FindDelimited(line, index + delimiter.size(), delimiter, &contentEnd)) {
                AppendTextSpan(spans, text);
                text.clear();
                AppendSpan(spans, doubled ? MarkdownSpanKind::Bold
                                          : MarkdownSpanKind::Italic,
                           std::string(line.substr(index + delimiter.size(),
                                                   contentEnd - index - delimiter.size())));
                index = contentEnd + delimiter.size();
                continue;
            }
        }
        if (ch == '[') {
            const std::size_t closeText = line.find(']', index + 1);
            if (closeText != std::string_view::npos && closeText + 1 < line.size() &&
                line[closeText + 1] == '(') {
                const std::size_t closeTarget = line.find(')', closeText + 2);
                if (closeTarget != std::string_view::npos) {
                    AppendTextSpan(spans, text);
                    text.clear();
                    MarkdownSpan span;
                    span.kind = MarkdownSpanKind::Link;
                    span.text = std::string(line.substr(index + 1, closeText - index - 1));
                    span.target = std::string(
                        line.substr(closeText + 2, closeTarget - closeText - 2));
                    spans.push_back(std::move(span));
                    index = closeTarget + 1;
                    continue;
                }
            }
        }
        text.push_back(ch);
        ++index;
    }
    AppendTextSpan(spans, text);
    return spans;
}

std::vector<MarkdownBlock> ParseMarkdown(std::string_view text) {
    const std::vector<std::string> lines = SplitLines(text);
    std::vector<MarkdownBlock> blocks;
    std::size_t index = 0;
    while (index < lines.size()) {
        const std::string_view line = lines[index];
        if (IsBlank(line)) {
            ++index;
            continue;
        }
        // 围栏代码块：原样吃到结尾的围栏
        std::string language;
        if (FenceMarker(line, &language)) {
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::Code;
            ++index;
            while (index < lines.size() && !FenceMarker(lines[index], nullptr)) {
                if (!block.code.empty()) {
                    block.code.push_back('\n');
                }
                block.code += lines[index];
                ++index;
            }
            if (index < lines.size()) {
                ++index;   // 跳过结尾的围栏
            }
            blocks.push_back(std::move(block));
            continue;
        }
        std::string_view body;
        if (const int level = HeadingLevel(line, &body); level > 0) {
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::Heading;
            block.level = level;
            block.spans = ParseInline(body);
            blocks.push_back(std::move(block));
            ++index;
            continue;
        }
        if (IsRule(line)) {
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::Rule;
            blocks.push_back(std::move(block));
            ++index;
            continue;
        }
        if (Trim(line).size() > 1 && Trim(line).front() == '>') {
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::Quote;
            std::string_view rest = Trim(Trim(line).substr(1));
            block.spans = ParseInline(Trim(rest));
            blocks.push_back(std::move(block));
            ++index;
            continue;
        }
        bool ordered = false;
        int number = 0;
        int level = 0;
        if (ListItem(line, &ordered, &number, &level, &body)) {
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::List;
            block.ordered = ordered;
            block.number = number;
            block.level = level;
            block.spans = ParseInline(body);
            blocks.push_back(std::move(block));
            ++index;
            continue;
        }
        // 其余：段落。连续的普通行合并成一段（CommonMark 也是这么做的）
        MarkdownBlock block;
        block.kind = MarkdownBlockKind::Paragraph;
        std::string paragraph;
        while (index < lines.size() && !IsBlank(lines[index])) {
            const std::string_view current = lines[index];
            std::string_view scratch;
            if (FenceMarker(current, nullptr) || HeadingLevel(current, &scratch) > 0 ||
                IsRule(current) || ListItem(current, &ordered, &number, &level, &scratch) ||
                (Trim(current).size() > 1 && Trim(current).front() == '>')) {
                break;   // 碰到下一个块的开始
            }
            if (!paragraph.empty()) {
                paragraph.push_back(' ');
            }
            paragraph += current;
            ++index;
        }
        block.spans = ParseInline(paragraph);
        blocks.push_back(std::move(block));
    }
    return blocks;
}

std::string SpanPlainText(const MarkdownSpan& span) {
    return span.text;
}

std::vector<std::string> BlockPlainLines(const MarkdownBlock& block) {
    std::vector<std::string> lines;
    switch (block.kind) {
        case MarkdownBlockKind::Code: {
            if (block.code.empty()) {
                lines.emplace_back();
                break;
            }
            std::size_t start = 0;
            while (start <= block.code.size()) {
                const std::size_t next = block.code.find('\n', start);
                if (next == std::string::npos) {
                    lines.push_back(block.code.substr(start));
                    break;
                }
                lines.push_back(block.code.substr(start, next - start));
                start = next + 1;
            }
            break;
        }
        case MarkdownBlockKind::Rule:
            lines.emplace_back();   // 分隔线在面板上是一条线，没有文字
            break;
        case MarkdownBlockKind::List: {
            std::string line = ListPrefixText(block.ordered, block.number);
            for (const MarkdownSpan& span : block.spans) {
                line += SpanPlainText(span);
            }
            lines.push_back(std::move(line));
            break;
        }
        default: {
            std::string line;
            if (block.kind == MarkdownBlockKind::Quote) {
                line = "\xE2\x94\x82 ";   // "│ "（UTF-8）
            }
            for (const MarkdownSpan& span : block.spans) {
                line += SpanPlainText(span);
            }
            lines.push_back(std::move(line));
            break;
        }
    }
    return lines;
}

std::string MarkdownToHtml(std::string_view text) {
    const std::vector<MarkdownBlock> blocks = ParseMarkdown(text);
    std::string body;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const MarkdownBlock& block = blocks[index];
        const auto inlineHtml = [](const std::vector<MarkdownSpan>& spans) {
            std::string out;
            for (const MarkdownSpan& span : spans) {
                switch (span.kind) {
                    case MarkdownSpanKind::Bold:
                        out += "<strong>" + EscapeHtmlText(span.text) + "</strong>";
                        break;
                    case MarkdownSpanKind::Italic:
                        out += "<em>" + EscapeHtmlText(span.text) + "</em>";
                        break;
                    case MarkdownSpanKind::Code:
                        out += "<code>" + EscapeHtmlText(span.text) + "</code>";
                        break;
                    case MarkdownSpanKind::Link:
                        out += "<a href=\"" + EscapeHtmlText(span.target) + "\">" +
                               EscapeHtmlText(span.text) + "</a>";
                        break;
                    default:
                        out += EscapeHtmlText(span.text);
                        break;
                }
            }
            return out;
        };
        switch (block.kind) {
            case MarkdownBlockKind::Heading: {
                const int level = std::min(std::max(block.level, 1), 6);
                const std::string tag = "h" + std::to_string(level);
                // 标题前面空一行：正文里"上一段紧接着标题"在 HTML 里会挤在一起
                if (index > 0) {
                    body += "\n";
                }
                body += "<" + tag + ">" + inlineHtml(block.spans) + "</" + tag + ">\n";
                break;
            }
            case MarkdownBlockKind::Code:
                body += "<pre><code>" + EscapeHtmlText(block.code) + "</code></pre>\n";
                break;
            case MarkdownBlockKind::Rule:
                body += "<hr>\n";
                break;
            case MarkdownBlockKind::Quote: {
                // 连续的引用合成一个 blockquote：一行一个 <blockquote> 在浏览器里会长出
                // 一条条竖线，看着像被拆成了好几段互不相干的引文
                std::string inner;
                std::size_t last = index;
                while (last < blocks.size() && blocks[last].kind == MarkdownBlockKind::Quote) {
                    if (!inner.empty()) {
                        inner += "<br>\n";
                    }
                    inner += inlineHtml(blocks[last].spans);
                    ++last;
                }
                body += "<blockquote>\n" + inner + "\n</blockquote>\n";
                index = last - 1;
                break;
            }
            case MarkdownBlockKind::List: {
                // 把连续的同一种列表合成一个 <ul>/<ol>；缩进层级用 class 表示
                const bool ordered = block.ordered;
                const char* tag = ordered ? "ol" : "ul";
                body += std::string("<") + tag + ">\n";
                std::size_t last = index;
                while (last < blocks.size() && blocks[last].kind == MarkdownBlockKind::List &&
                       blocks[last].ordered == ordered) {
                    const int level = std::max(blocks[last].level, 0);
                    body += "  <li class=\"l" + std::to_string(level) + "\">" +
                            inlineHtml(blocks[last].spans) + "</li>\n";
                    ++last;
                }
                body += std::string("</") + tag + ">\n";
                index = last - 1;
                break;
            }
            default:
                body += "<p>" + inlineHtml(block.spans) + "</p>\n";
                break;
        }
    }
    // 独立的 HTML：带一小段内嵌样式，任何浏览器打开都能看
    std::string document;
    document += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    document += "<title>notepadcc Markdown 预览</title>\n";
    document += "<style>\n"
                "body { font-family: 'Microsoft YaHei UI', 'Segoe UI', sans-serif; "
                "max-width: 46em; margin: 2em auto; padding: 0 1em; line-height: 1.6; "
                "color: #1E1E1E; background: #FFFFFF; }\n"
                "h1,h2,h3,h4,h5,h6 { line-height: 1.3; margin: 1.2em 0 0.5em; }\n"
                "code { font-family: Consolas, monospace; background: #F2F2F2; "
                "padding: 0.1em 0.3em; border-radius: 3px; }\n"
                "pre { background: #F6F8FA; border: 1px solid #E0E0E0; border-radius: 4px; "
                "padding: 0.8em; overflow: auto; }\n"
                "pre code { background: none; padding: 0; }\n"
                "blockquote { margin: 0.8em 0; padding: 0.2em 0.9em; "
                "border-left: 4px solid #D0D7DE; color: #57606A; }\n"
                "li.l1 { margin-left: 1.4em; } li.l2 { margin-left: 2.8em; }\n"
                "li.l3 { margin-left: 4.2em; }\n"
                "a { color: #0969DA; }\n"
                "hr { border: none; border-top: 1px solid #D0D7DE; margin: 1.4em 0; }\n"
                "</style>\n</head>\n<body>\n";
    document += body;
    document += "</body>\n</html>\n";
    return document;
}

}  // namespace npcc::core
