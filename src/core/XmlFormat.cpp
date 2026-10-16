#include "core/XmlFormat.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace npcc::core {
namespace {

struct Token {
    enum class Kind { Open, Close, SelfClosing, Text, Comment, CData, PI, Doctype };

    Kind         kind = Kind::Text;
    std::size_t  begin = 0;
    std::size_t  end = 0;      // 不含结尾（raw 用 substr(begin, end-begin) 取）
    std::string  name;         // 标签名（只有 Open/Close/SelfClosing 有）
    // 内容只由"元素标记 / 注释 / 处理指令 / 空白"组成，且**直接子节点里没有裸文本、
    // 没有 CDATA**。只有这样的元素才允许重排缩进——重排等于往它的内容里塞空白。
    //
    // 判断只看直接子节点：`<a><b>文字</b></a>` 里的 a 仍算纯元素内容，
    // 因为文字在 b 里面，a 自己没被塞东西；b 会整段原样搬运。
    bool         elementOnly = false;
    std::size_t  match = 0;    // Open 对应的 Close 的 token 下标
};

bool IsNameStart(unsigned char ch) {
    return std::isalpha(ch) != 0 || ch == '_' || ch == ':' || ch >= 0x80;
}

bool IsNameBody(unsigned char ch) {
    return IsNameStart(ch) || std::isdigit(ch) != 0 || ch == '-' || ch == '.';
}

std::wstring Where(std::string_view text, std::size_t offset) {
    int line = 1;
    int column = 1;
    for (std::size_t index = 0; index < offset && index < text.size(); ++index) {
        if (text[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return L"第 " + std::to_wstring(line) + L" 行第 " + std::to_wstring(column) + L" 列：";
}

std::string ToNarrow(std::string_view text) { return std::string(text); }

// 词法：把文档切成标记与文本。失败时填 error 并返回 false。
bool Tokenize(std::string_view text, std::vector<Token>& out, std::wstring& error) {
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] != '<') {
            const std::size_t begin = index;
            while (index < text.size() && text[index] != '<') {
                ++index;
            }
            Token token;
            token.kind = Token::Kind::Text;
            token.begin = begin;
            token.end = index;
            out.push_back(std::move(token));
            continue;
        }
        // 各种 "以 < 开头" 的标记
        const auto startsWith = [&](std::string_view prefix) {
            return text.compare(index, prefix.size(), prefix) == 0;
        };
        if (startsWith("<!--")) {
            const std::size_t close = text.find("-->", index + 4);
            if (close == std::string_view::npos) {
                error = Where(text, index) + L"注释没有收尾（缺 -->）";
                return false;
            }
            Token token;
            token.kind = Token::Kind::Comment;
            token.begin = index;
            token.end = close + 3;
            out.push_back(std::move(token));
            index = close + 3;
            continue;
        }
        if (startsWith("<![CDATA[")) {
            const std::size_t close = text.find("]]>", index + 9);
            if (close == std::string_view::npos) {
                error = Where(text, index) + L"CDATA 没有收尾（缺 ]]>）";
                return false;
            }
            Token token;
            token.kind = Token::Kind::CData;
            token.begin = index;
            token.end = close + 3;
            out.push_back(std::move(token));
            index = close + 3;
            continue;
        }
        if (startsWith("<?")) {
            const std::size_t close = text.find("?>", index + 2);
            if (close == std::string_view::npos) {
                error = Where(text, index) + L"处理指令没有收尾（缺 ?>）";
                return false;
            }
            Token token;
            token.kind = Token::Kind::PI;
            token.begin = index;
            token.end = close + 2;
            out.push_back(std::move(token));
            index = close + 2;
            continue;
        }
        if (startsWith("<!DOCTYPE") || startsWith("<!doctype")) {
            // 内部子集 `[ ... ]` 里也会有 `>`，要一起吃掉
            std::size_t cursor = index + 9;
            int subsetDepth = 0;
            while (cursor < text.size()) {
                const char ch = text[cursor];
                if (ch == '[') {
                    ++subsetDepth;
                } else if (ch == ']') {
                    subsetDepth = std::max(0, subsetDepth - 1);
                } else if (ch == '>' && subsetDepth == 0) {
                    break;
                }
                ++cursor;
            }
            if (cursor >= text.size()) {
                error = Where(text, index) + L"DOCTYPE 没有收尾";
                return false;
            }
            Token token;
            token.kind = Token::Kind::Doctype;
            token.begin = index;
            token.end = cursor + 1;
            out.push_back(std::move(token));
            index = cursor + 1;
            continue;
        }
        // 开标签 / 闭标签 / 自闭合标签
        const bool closing = startsWith("</");
        std::size_t cursor = index + (closing ? 2 : 1);
        if (cursor >= text.size() || !IsNameStart(static_cast<unsigned char>(text[cursor]))) {
            error = Where(text, index) + L"这里不是一个合法的标签名";
            return false;
        }
        const std::size_t nameBegin = cursor;
        while (cursor < text.size() && IsNameBody(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        const std::string name = ToNarrow(text.substr(nameBegin, cursor - nameBegin));
        bool selfClosing = false;
        bool inQuote = false;
        char quote = '\0';
        for (; cursor < text.size(); ++cursor) {
            const char ch = text[cursor];
            if (inQuote) {
                if (ch == quote) {
                    inQuote = false;
                }
                continue;
            }
            if (ch == '"' || ch == '\'') {
                inQuote = true;
                quote = ch;
                continue;
            }
            if (ch == '<') {
                error = Where(text, cursor) + L"标签里出现了 '<'（属性值忘了加引号？）";
                return false;
            }
            if (ch == '>') {
                break;
            }
        }
        if (cursor >= text.size() || text[cursor] != '>') {
            error = Where(text, index) + L"标签没有收尾（缺 >）";
            return false;
        }
        if (cursor > nameBegin) {
            std::size_t tail = cursor;
            while (tail > nameBegin && (text[tail - 1] == ' ' || text[tail - 1] == '\t' ||
                                        text[tail - 1] == '\r' || text[tail - 1] == '\n')) {
                --tail;
            }
            selfClosing = tail > nameBegin && text[tail - 1] == '/';
        }
        Token token;
        token.kind = closing ? Token::Kind::Close
                             : (selfClosing ? Token::Kind::SelfClosing : Token::Kind::Open);
        token.begin = index;
        token.end = cursor + 1;
        token.name = name;
        out.push_back(std::move(token));
        index = cursor + 1;
    }
    return true;
}

}  // namespace

FormatResult FormatXml(std::string_view text, int indent) {
    const int step = std::max(0, std::min(16, indent));
    std::vector<Token> tokens;
    std::wstring error;
    if (!Tokenize(text, tokens, error)) {
        return FormatResult::Fail(std::move(error));
    }
    if (tokens.empty()) {
        return FormatResult::Fail(L"内容为空，没有可格式化的 XML");
    }

    // 配对 + 判断"内容是不是纯元素"。
    // mixed[i] 表示第 i 个 token（必须是个开标签）的**直接子节点**里有裸文本或 CDATA。
    // 只记直接子节点：孙辈里的文字不该把祖先也拖下水，否则一份"每个叶子都带文字"的
    // 正常 XML（几乎是全部 XML）会被整体判成混合内容，缩进一个字都排不了。
    std::vector<std::size_t> stack;
    std::vector<bool> mixed(tokens.size(), false);
    bool sawContent = false;   // 有没有"不只是空白"的东西；全是空白就等于空文档
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        Token& token = tokens[index];
        if (token.kind == Token::Kind::Open) {
            stack.push_back(index);
            sawContent = true;
            continue;
        }
        if (token.kind == Token::Kind::SelfClosing || token.kind == Token::Kind::PI ||
            token.kind == Token::Kind::Comment || token.kind == Token::Kind::CData ||
            token.kind == Token::Kind::Doctype) {
            sawContent = true;
            // CDATA 的内容是**字符数据**：它所在的元素算混合内容，整段不动
            if (token.kind == Token::Kind::CData && !stack.empty()) {
                mixed[stack.back()] = true;
            }
            continue;
        }
        if (token.kind == Token::Kind::Close) {
            sawContent = true;
            if (stack.empty()) {
                return FormatResult::Fail(Where(text, token.begin) + L"多了一个结束标签 </" +
                                          std::wstring(token.name.begin(), token.name.end()) + L">");
            }
            const std::size_t openIndex = stack.back();
            stack.pop_back();
            Token& open = tokens[openIndex];
            if (open.name != token.name) {
                return FormatResult::Fail(
                    Where(text, token.begin) + L"结束标签 </" +
                    std::wstring(token.name.begin(), token.name.end()) + L"> 与 <" +
                    std::wstring(open.name.begin(), open.name.end()) + L"> 不匹配");
            }
            open.match = index;
            open.elementOnly = !mixed[openIndex];
            continue;
        }
        // 文本：非空白就算"裸文本"
        bool blank = true;
        for (std::size_t cursor = token.begin; cursor < token.end; ++cursor) {
            const char ch = text[cursor];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                blank = false;
                break;
            }
        }
        if (!blank) {
            sawContent = true;
            if (!stack.empty()) {
                mixed[stack.back()] = true;
            }
        }
    }
    if (!sawContent) {
        return FormatResult::Fail(L"内容为空，没有可格式化的 XML");
    }
    if (!stack.empty()) {
        const Token& open = tokens[stack.back()];
        return FormatResult::Fail(Where(text, open.begin) + L"<" +
                                  std::wstring(open.name.begin(), open.name.end()) +
                                  L"> 没有对应的结束标签");
    }

    // 输出
    std::string out;
    int depth = 0;
    const auto emitIndent = [&out, step](int level) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        out.append(static_cast<std::size_t>(std::max(0, level) * step), ' ');
    };
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const Token& token = tokens[index];
        switch (token.kind) {
            case Token::Kind::Text: {
                // 纯空白文本只在被跳过时才有意义：这里什么都不输出（缩进由我们自己给）。
                // 有内容的文本属于混合内容，会在父元素那里整段搬运，不会走到这儿
                bool blank = true;
                for (std::size_t cursor = token.begin; cursor < token.end; ++cursor) {
                    const char ch = text[cursor];
                    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                        blank = false;
                        break;
                    }
                }
                if (!blank) {
                    out.append(text.substr(token.begin, token.end - token.begin));
                }
                break;
            }
            case Token::Kind::Open: {
                if (!token.elementOnly) {
                    // 混合内容：从开标签到结束标签**整段原样搬运**，一个字节都不动
                    const Token& close = tokens[token.match];
                    emitIndent(depth);
                    out.append(text.substr(token.begin, close.end - token.begin));
                    index = token.match;
                    break;
                }
                // 空元素（一对标签之间**一个 token 都没有**）：压成一行 <a></a>。
                // 注意判据是"一个 token 都没有"，不是"没有子元素"：`<a>   </a>` 中间
                // 那串空格是实实在在的字符数据，压掉它就是在改内容。
                if (index + 1 == token.match) {
                    emitIndent(depth);
                    out.append(text.substr(token.begin, token.end - token.begin));
                    out.append(text.substr(tokens[token.match].begin,
                                           tokens[token.match].end - tokens[token.match].begin));
                    index = token.match;
                    break;
                }
                emitIndent(depth);
                out.append(text.substr(token.begin, token.end - token.begin));
                ++depth;
                break;
            }
            case Token::Kind::Close:
                depth = std::max(0, depth - 1);
                emitIndent(depth);
                out.append(text.substr(token.begin, token.end - token.begin));
                break;
            default:
                emitIndent(depth);
                out.append(text.substr(token.begin, token.end - token.begin));
                break;
        }
    }
    out.push_back('\n');
    return FormatResult::Ok(std::move(out));
}

}  // namespace npcc::core
