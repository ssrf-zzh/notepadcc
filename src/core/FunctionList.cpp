#include "core/FunctionList.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace npcc::core {
namespace {

enum class Rule {
    None,
    Brace,       // C 家族 / JS / TS：靠大括号深度 + "名字(" 判断
    Indent,      // Python：靠缩进层级 + def/class
    Heading,     // Markdown：# 的个数就是层级
    Selector,    // CSS：花括号前的选择器
};

Rule RuleFor(std::wstring_view language) {
    if (language == L"cpp" || language == L"javascript" || language == L"typescript") {
        return Rule::Brace;
    }
    if (language == L"python") {
        return Rule::Indent;
    }
    if (language == L"markdown") {
        return Rule::Heading;
    }
    if (language == L"css") {
        return Rule::Selector;
    }
    return Rule::None;
}

bool IsWordStart(unsigned char ch) {
    return std::isalpha(ch) != 0 || ch == '_' || ch >= 0x80;
}

bool IsWordBody(unsigned char ch) {
    return IsWordStart(ch) || std::isdigit(ch) != 0;
}

// 逐行扫：把每行的行首偏移记下来（行号是"跳转到哪一行"的依据）
struct Line {
    std::size_t begin = 0;
    std::size_t end = 0;    // 不含行尾换行
    int         number = 0;
};

std::vector<Line> SplitLines(std::string_view text) {
    std::vector<Line> lines;
    std::size_t index = 0;
    int number = 0;
    while (index <= text.size()) {
        const std::size_t begin = index;
        while (index < text.size() && text[index] != '\n' && text[index] != '\r') {
            ++index;
        }
        lines.push_back(Line{begin, index, number++});
        if (index >= text.size()) {
            break;
        }
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
            index += 2;
        } else {
            ++index;
        }
    }
    return lines;
}

std::wstring Widen(std::string_view bytes) {
    // 提取到的是 UTF-8 字节，转成宽字符给界面用
    if (bytes.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                                           nullptr, 0);
    if (need <= 0) {
        // 不是合法 UTF-8（比如 ANSI 文档）：按字节原样搬过去，至少不是空的
        std::wstring out;
        out.reserve(bytes.size());
        for (const char ch : bytes) {
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
        }
        return out;
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(),
                          need);
    return out;
}

// 一行的"有效内容"：去掉行首空白，并记下缩进宽度
struct LineView {
    std::size_t indent = 0;
    std::size_t start = 0;   // 第一个非空白字节（相对整篇）
    std::size_t end = 0;
};

LineView TrimLine(std::string_view text, const Line& line) {
    LineView view;
    view.start = line.begin;
    while (view.start < line.end &&
           (text[view.start] == ' ' || text[view.start] == '\t')) {
        view.indent += text[view.start] == '\t' ? 4 : 1;
        ++view.start;
    }
    view.end = line.end;
    return view;
}

bool MatchWord(std::string_view text, std::size_t pos, std::string_view word) {
    if (pos + word.size() > text.size()) {
        return false;
    }
    return text.compare(pos, word.size(), word) == 0;
}

// ---- C 家族 / JS / TS ----
//
// 规则（够用就好）：一行里出现"名字("且这一行不是注释/预处理指令，就当成函数定义；
// 出现 class/struct/namespace/interface/enum + 名字，就当成容器。层级按大括号深度算。
//
// 大括号深度必须**跳过字符串与注释里的括号**，否则 `printf("}")` 会把层级算歪——
// 这一点在本项目的测试里专门钉了（见设计说明书 9.2）。
// 扫一行，更新大括号深度。跳过字符串与注释里的括号——`printf("}")` 这种要是算了，
// 后面的层级会整体错一层（测试里专门钉了这一条）
void ScanBraces(std::string_view content, int& depth, bool& inBlockComment, bool& inString,
                char& stringDelim) {
    for (std::size_t i = 0; i < content.size(); ++i) {
        const char ch = content[i];
        const char next = i + 1 < content.size() ? content[i + 1] : '\0';
        if (inBlockComment) {
            if (ch == '*' && next == '/') {
                inBlockComment = false;
                ++i;
            }
            continue;
        }
        if (inString) {
            if (ch == '\\') {
                ++i;   // 转义：跳过下一个字符
            } else if (ch == stringDelim) {
                inString = false;
            }
            continue;
        }
        if (ch == '/' && next == '/') {
            return;   // 行注释：这一行后面都不算
        }
        if (ch == '/' && next == '*') {
            inBlockComment = true;
            ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = true;
            stringDelim = ch;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            depth = depth > 0 ? depth - 1 : 0;
        }
    }
}

// 在这一行里找一个"定义"：返回名字与类别。找不到就返回 false。
//
// 关键是**别把调用语句当定义**：`printf("}");` 与 `void draw() {` 长得几乎一样。
// 判据是"名字后面那对括号之后"是什么：紧跟 `{`（或这一行到此为止、下一行以 `{` 开头）
// 才算定义；先碰到 `;` 就是语句。
bool FindDefinition(std::string_view line, std::string_view nextLine, std::wstring& name,
                    std::wstring& kind) {
    const std::string_view trimmed = line;
    if (trimmed.empty() || trimmed[0] == '#') {
        return false;
    }
    // 容器关键字：class / struct / namespace / interface / enum / union / trait
    static const char* kContainers[] = {"class ", "struct ", "namespace ", "interface ",
                                        "enum ",  "union ",  "trait "};
    for (const char* keyword : kContainers) {
        const std::size_t at = trimmed.find(keyword);
        if (at == std::string_view::npos) {
            continue;
        }
        std::size_t cursor = at + ::strlen(keyword);
        while (cursor < trimmed.size() &&
               !IsWordStart(static_cast<unsigned char>(trimmed[cursor]))) {
            ++cursor;
        }
        std::size_t nameEnd = cursor;
        while (nameEnd < trimmed.size() &&
               IsWordBody(static_cast<unsigned char>(trimmed[nameEnd]))) {
            ++nameEnd;
        }
        if (nameEnd > cursor) {
            name = Widen(trimmed.substr(cursor, nameEnd - cursor));
            kind = L"类";
            return true;
        }
    }
    // 函数：取第一个 `(` 前面那个标识符（行首通常是返回类型，不是名字）
    const std::size_t paren = trimmed.find('(');
    if (paren == std::string_view::npos || paren == 0) {
        return false;
    }
    if (const std::size_t semicolon = trimmed.find(';');
        semicolon != std::string_view::npos && semicolon < paren) {
        return false;
    }
    std::size_t nameEnd = paren;
    while (nameEnd > 0 && (trimmed[nameEnd - 1] == ' ' || trimmed[nameEnd - 1] == '\t')) {
        --nameEnd;
    }
    std::size_t cursor = nameEnd;
    while (cursor > 0 && IsWordBody(static_cast<unsigned char>(trimmed[cursor - 1]))) {
        --cursor;
    }
    if (cursor == nameEnd || !IsWordStart(static_cast<unsigned char>(trimmed[cursor]))) {
        return false;   // `(x)` 这类
    }
    if (cursor >= 2 && (trimmed[cursor - 1] == '.' ||
                        (trimmed[cursor - 1] == '>' && trimmed[cursor - 2] == '-'))) {
        return false;   // obj.method( / ptr->method(
    }
    const std::string_view candidate = trimmed.substr(cursor, nameEnd - cursor);
    static const char* kNotFunctions[] = {"if",   "for",    "while", "switch", "catch",
                                          "return", "sizeof", "new",   "delete", "throw"};
    for (const char* word : kNotFunctions) {
        if (candidate == word) {
            return false;
        }
    }
    // 括号之后：先碰到 `;` 就是语句；碰到 `{` 才是定义；这一行到末尾还没有，
    // 就看下一行是不是以 `{` 开头（K&R 之外的换行写法）
    std::size_t probe = paren;
    int parenDepth = 0;
    bool sawBrace = false;
    for (; probe < trimmed.size(); ++probe) {
        const char ch = trimmed[probe];
        if (ch == '(') {
            ++parenDepth;
        } else if (ch == ')') {
            --parenDepth;
            if (parenDepth == 0) {
                ++probe;
                break;
            }
        } else if (ch == ';') {
            return false;
        } else if (ch == '{') {
            sawBrace = true;
            break;
        }
    }
    if (!sawBrace) {
        for (; probe < trimmed.size(); ++probe) {
            const char ch = trimmed[probe];
            if (ch == ';') {
                return false;
            }
            if (ch == '{') {
                sawBrace = true;
                break;
            }
        }
    }
    if (!sawBrace) {
        std::size_t begin = 0;
        while (begin < nextLine.size() &&
               (nextLine[begin] == ' ' || nextLine[begin] == '\t')) {
            ++begin;
        }
        sawBrace = begin < nextLine.size() && nextLine[begin] == '{';
    }
    if (!sawBrace) {
        return false;   // 声明（`int f(int);`）或者调用（`printf(…);`）
    }
    name = Widen(candidate);
    kind = L"函数";
    return true;
}

std::vector<FunctionSymbol> ExtractBrace(std::string_view text, const std::vector<Line>& lines) {
    std::vector<FunctionSymbol> symbols;
    int depth = 0;
    bool inBlockComment = false;
    bool inString = false;
    char stringDelim = '\0';
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const Line& line = lines[index];
        const LineView view = TrimLine(text, line);
        const std::string_view content = text.substr(view.start, view.end - view.start);
        std::string_view nextContent;
        if (index + 1 < lines.size()) {
            const LineView nextView = TrimLine(text, lines[index + 1]);
            nextContent = text.substr(nextView.start, nextView.end - nextView.start);
        }

        // 符号的层级 = 它在**这一行开头**所处的层级（自己那个 `{` 不算进去，
        // 否则顶层函数会被记成 1 层）
        if (!inBlockComment && !inString) {
            std::wstring name;
            std::wstring kind;
            if (FindDefinition(content, nextContent, name, kind)) {
                FunctionSymbol symbol;
                symbol.name = std::move(name);
                symbol.line = line.number;
                symbol.depth = depth;
                symbol.kind = std::move(kind);
                symbols.push_back(std::move(symbol));
            }
        }
        // 再更新层级（自己这个 `{` 从这里开始生效）
        ScanBraces(content, depth, inBlockComment, inString, stringDelim);
        if (!inBlockComment && !inString) {
            // 行尾没闭合的块注释/字符串由下一行接着算，这里不用额外处理
        }
    }
    return symbols;
}


// ---- Python：靠缩进 ----
std::vector<FunctionSymbol> ExtractIndent(std::string_view text, const std::vector<Line>& lines) {
    std::vector<FunctionSymbol> symbols;
    std::vector<std::size_t> indentStack;   // 每一层的缩进宽度
    for (const Line& line : lines) {
        const LineView view = TrimLine(text, line);
        if (view.start >= view.end || text[view.start] == '#') {
            continue;   // 空行与整行注释不影响层级
        }
        const std::string_view content = text.substr(view.start, view.end - view.start);
        const bool isDef = MatchWord(content, 0, "def ") || MatchWord(content, 0, "async def ") ||
                           MatchWord(content, 0, "class ");
        if (!isDef) {
            continue;
        }
        // 弹掉比当前缩进更深的层
        while (!indentStack.empty() && indentStack.back() >= view.indent) {
            indentStack.pop_back();
        }
        const std::size_t keywordLength = MatchWord(content, 0, "async def ")
                                              ? 10
                                              : (MatchWord(content, 0, "def ") ? 4 : 6);
        std::size_t cursor = keywordLength;
        std::size_t nameEnd = cursor;
        while (nameEnd < content.size() &&
               IsWordBody(static_cast<unsigned char>(content[nameEnd]))) {
            ++nameEnd;
        }
        if (nameEnd == cursor) {
            continue;
        }
        FunctionSymbol symbol;
        symbol.name = Widen(content.substr(cursor, nameEnd - cursor));
        symbol.line = line.number;
        symbol.depth = static_cast<int>(indentStack.size());
        symbol.kind = MatchWord(content, 0, "class ") ? L"类" : L"函数";
        symbols.push_back(std::move(symbol));
        indentStack.push_back(view.indent);
    }
    return symbols;
}

// ---- Markdown：标题层级 ----
std::vector<FunctionSymbol> ExtractHeading(std::string_view text, const std::vector<Line>& lines) {
    std::vector<FunctionSymbol> symbols;
    bool inFence = false;
    for (const Line& line : lines) {
        const LineView view = TrimLine(text, line);
        const std::string_view content = text.substr(view.start, view.end - view.start);
        // 代码块里的 # 是代码不是标题
        if (content.size() >= 3 && (content.compare(0, 3, "```") == 0)) {
            inFence = !inFence;
            continue;
        }
        if (inFence || content.empty() || content[0] != '#') {
            continue;
        }
        std::size_t hashes = 0;
        while (hashes < content.size() && content[hashes] == '#') {
            ++hashes;
        }
        if (hashes > 6 || hashes >= content.size() || content[hashes] != ' ') {
            continue;   // "#hashtag" 不是标题
        }
        std::size_t begin = hashes;
        while (begin < content.size() && content[begin] == ' ') {
            ++begin;
        }
        std::size_t end = content.size();
        while (end > begin && (content[end - 1] == ' ' || content[end - 1] == '#')) {
            --end;
        }
        FunctionSymbol symbol;
        symbol.name = Widen(content.substr(begin, end - begin));
        symbol.line = line.number;
        symbol.depth = static_cast<int>(hashes) - 1;
        symbol.kind = L"标题";
        symbols.push_back(std::move(symbol));
    }
    return symbols;
}

// ---- CSS：花括号之前的选择器 ----
std::vector<FunctionSymbol> ExtractSelector(std::string_view text, const std::vector<Line>& lines) {
    std::vector<FunctionSymbol> symbols;
    int depth = 0;
    bool inComment = false;
    for (const Line& line : lines) {
        const LineView view = TrimLine(text, line);
        std::string_view content = text.substr(view.start, view.end - view.start);
        const std::size_t brace = content.find('{');
        // 层级要按"选择器自己那个左括号**之前**的括号"来算：
        // `.inner { color: blue; }` 这一行里既有 `{` 又有 `}`，整行一起算会把它自己
        // 的括号也算进去，层级就少一层（媒体查询里的选择器会因此显示成顶层）
        const std::size_t scanTo = brace == std::string_view::npos ? content.size() : brace + 1;
        for (std::size_t i = 0; i < scanTo; ++i) {
            if (inComment) {
                if (content[i] == '*' && i + 1 < content.size() && content[i + 1] == '/') {
                    inComment = false;
                    ++i;
                }
                continue;
            }
            if (content[i] == '/' && i + 1 < content.size() && content[i + 1] == '*') {
                inComment = true;
                ++i;
                continue;
            }
            if (content[i] == '{') {
                ++depth;
            } else if (content[i] == '}') {
                depth = depth > 0 ? depth - 1 : 0;
            }
        }
        if (brace == std::string_view::npos || inComment) {
            // 这一行没有 `{`：把剩下的括号（比如 `}`）接着扫完，层级才准
            for (std::size_t i = scanTo; i < content.size(); ++i) {
                if (content[i] == '}') {
                    depth = depth > 0 ? depth - 1 : 0;
                } else if (content[i] == '{') {
                    ++depth;
                }
            }
            continue;
        }
        std::string_view selector = content.substr(0, brace);
        while (!selector.empty() && (selector.back() == ' ' || selector.back() == '\t')) {
            selector.remove_suffix(1);
        }
        if (selector.empty() || selector[0] == '@') {
            continue;   // @media 之类不是选择器（列出来只是噪音）
        }
        FunctionSymbol symbol;
        symbol.name = Widen(selector);
        symbol.line = line.number;
        symbol.depth = depth > 0 ? depth - 1 : 0;   // 减掉它自己那个 `{`
        symbol.kind = L"选择器";
        symbols.push_back(std::move(symbol));
        // 本行 `{` 之后的括号（一行里写完整条规则时会有 `}`）也要算进层级
        for (std::size_t i = scanTo; i < content.size(); ++i) {
            if (content[i] == '}') {
                depth = depth > 0 ? depth - 1 : 0;
            } else if (content[i] == '{') {
                ++depth;
            }
        }
    }
    return symbols;
}

}  // namespace

bool HasFunctionExtractor(std::wstring_view language) {
    return RuleFor(language) != Rule::None;
}

std::vector<FunctionSymbol> ExtractFunctionList(std::wstring_view language,
                                                std::string_view text) {
    const Rule rule = RuleFor(language);
    if (rule == Rule::None || text.empty()) {
        return {};
    }
    const std::vector<Line> lines = SplitLines(text);
    switch (rule) {
        case Rule::Brace:
            return ExtractBrace(text, lines);
        case Rule::Indent:
            return ExtractIndent(text, lines);
        case Rule::Heading:
            return ExtractHeading(text, lines);
        case Rule::Selector:
            return ExtractSelector(text, lines);
        case Rule::None:
            break;
    }
    return {};
}

}  // namespace npcc::core
