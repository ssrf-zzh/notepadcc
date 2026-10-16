#include "edit/UdlLexer.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <string>
#include <vector>

#include <Scintilla.h>   // SC_FOLDLEVEL*（折叠层级用的位）
#include <SciLexer.h>

// 这几个 Lexilla 头文件**必须按这个顺序**包含：Accessor.h 里的
// `class Accessor : public LexAccessor` 不自带 LexAccessor.h，LexAccessor.h 又用了
// assert 却不包含 <cassert>。照官方词法器的顺序抄，别按字母序排。
#include "ILexer.h"
#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "PropSetSimple.h"
#include "LexerBase.h"

#include "edit/UdlStyles.h"

// UDL 词法器。
//
// 为什么是自研的：Lexilla 5.5.4 里没有 container 词法器（设计说明书 7.6 末尾记了这次
// 改判），"把 UDL 编译成关键词表 + 属性串去复用容器词法器"这条路走不通。所以这里直接
// 实现 Scintilla 的 ILexer5（借 Lexilla 的 LexerBase 兜掉那些样板方法），扫描时用
// StyleContext / Accessor 这套 Lexilla 内部工具——风格上与官方词法器一致，改起来不至于
// 只有我们看得懂。
//
// 责任划分：关键字表由外壳经 SCI_SETKEYWORDS 交给这里（WordListSet），这样做的好处是
// "切换语言时 0~8 组都要显式设一遍"这条既有纪律自动对所有语言都成立；样式号与配色由
// UdlStyles.h 与 EditView 的配色表负责。

namespace npcc::edit {
namespace {

using Lexilla::Accessor;
using Lexilla::LexerBase;
using Lexilla::StyleContext;

constexpr Sci_PositionU  kMaxKeywordGroup = static_cast<Sci_PositionU>(udl::kKeywordGroupCount);
// 收词用的缓冲区。与 Lexilla 官方词法器一致取 128 字节：超长标识符会被截断，
// 截断后不会match 到任何关键字（关键字没有这么长的），所以只是"不着色"，不会错着色。
constexpr std::size_t    kWordBufferSize = 128;

bool IsAsciiDigit(char ch) {
    return ch >= '0' && ch <= '9';
}

bool IsAsciiLetter(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

// 词的起点：字母、下划线或高位字节（UTF-8 的中文之类也算词，这与文档内提词的口径一致）
bool IsWordStart(unsigned char byte) {
    return IsAsciiLetter(static_cast<char>(byte)) || byte == '_' || byte >= 0x80;
}

bool IsWordBody(unsigned char byte) {
    return IsWordStart(byte) || IsAsciiDigit(static_cast<char>(byte));
}

// 上面两个是给"按字节扫"的地方用的（Fold 与记号比对）。下面两个是给 StyleContext
// 用的：它给的 sc.ch 是**码点**不是字节，把码点强转成 unsigned char 会把中文这类字符
// 截成别的字符（U+4E2D 截出来是 0x2D，也就是 '-'，于是汉字会被当成运算符）。
bool IsWordStartChar(int ch) {
    if (ch <= 0) {
        return false;
    }
    if (ch >= 0x80) {
        return true;
    }
    return IsAsciiLetter(static_cast<char>(ch)) || ch == '_';
}

bool IsWordBodyChar(int ch) {
    if (ch >= 0x80) {
        return true;
    }
    return IsAsciiLetter(static_cast<char>(ch)) || ch == '_' ||
           IsAsciiDigit(static_cast<char>(ch));
}

// 行注释符前面必须是行首或空白：这样 "a // b" 里的 // 算注释，而 "http://x" 这种
// 夹在词中间的不算。StyleContext 在行首时 chPrev 是换行符，所以一条规则就够。
bool AtTokenBoundary(const StyleContext& sc) {
    if (sc.atLineStart) {
        return true;
    }
    return sc.chPrev == ' ' || sc.chPrev == '\t' || sc.chPrev == '\r' ||
           sc.chPrev == '\n' || sc.chPrev == '\v' || sc.chPrev == '\f';
}

bool IsOperatorChar(std::string_view operators, unsigned char byte) {
    return operators.find(static_cast<char>(byte)) != std::string_view::npos;
}

// 按字节比对当前位置上的记号（不依赖 StyleContext 的字符宽度，ASCII 记号也够用）
bool MatchBytes(Lexilla::LexAccessor& styler, Sci_Position pos, std::string_view token) {
    if (token.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (styler.SafeGetCharAt(pos + static_cast<Sci_Position>(i), '\0') != token[i]) {
            return false;
        }
    }
    return true;
}

class UdlLexer final : public LexerBase {
public:
    explicit UdlLexer(const UdlDefinition& definition) : definition_(definition) {
        name_ = "udl:";
        for (const wchar_t ch : definition.name) {
            name_.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
        }
    }

    UdlLexer(const UdlLexer&) = delete;
    UdlLexer& operator=(const UdlLexer&) = delete;
    ~UdlLexer() override = default;

    const char* SCI_METHOD GetName() override { return name_.c_str(); }
    // 自研词法器没有 SCLEX_* 编号。Scintilla 只用它回答 SCI_GETLEXER，不参与分派。
    int SCI_METHOD GetIdentifier() override { return 0; }

    Sci_Position SCI_METHOD WordListSet(int n, const char* wl) override {
        if (n < 0 || n >= static_cast<int>(kMaxKeywordGroup)) {
            return -1;
        }
        if (keyWordLists[n] == nullptr) {
            return -1;
        }
        // 关键字大小写：WordList 的比对是逐字节的，所以"不区分大小写"要靠把词表与
        // 待查词**都**转成小写来做（Set 的第二个参数就是干这件事）。
        keyWordLists[n]->Set(wl != nullptr ? wl : "", !definition_.caseSensitive);
        return 0;
    }

    void SCI_METHOD Lex(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle,
                        Scintilla::IDocument* pAccess) override;

    void SCI_METHOD Fold(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle,
                         Scintilla::IDocument* pAccess) override;

private:
    // 当前字符串用的界定符（起始处确定，离开字符串时清掉）。字符串不跨行，所以它
    // 不需要活过一次 Lex 调用——中途开始着色时在本行内往回找一次就够。
    struct ActiveString {
        std::string end;
        std::string escape;
    };

    ActiveString RecoverString(Accessor& styler, Sci_Position pos) const;
    bool StartString(Accessor& styler, Sci_Position pos, ActiveString& out) const;
    int  KeywordStyleOf(const char* word) const;

    UdlDefinition definition_;
    std::string   name_;
};

UdlLexer::ActiveString UdlLexer::RecoverString(Accessor& styler, Sci_Position pos) const {
    // 从当前位置按**字节**往回走，直到不再是字符串样式：那就是字符串的起始界定符。
    // 字符串不跨行，所以最多回退到行首。
    const Sci_Position lineStart = styler.LineStart(styler.GetLine(pos));
    Sci_Position start = pos;
    while (start > lineStart && styler.StyleAt(start - 1) == udl::kStyleString) {
        --start;
    }
    for (const UdlStringDelim& delim : definition_.strings) {
        if (MatchBytes(styler, start, delim.start)) {
            return ActiveString{delim.end, delim.escape};
        }
    }
    return ActiveString{};
}

bool UdlLexer::StartString(Accessor& styler, Sci_Position pos, ActiveString& out) const {
    for (const UdlStringDelim& delim : definition_.strings) {
        if (MatchBytes(styler, pos, delim.start)) {
            out.end = delim.end;
            out.escape = delim.escape;
            return true;
        }
    }
    return false;
}

int UdlLexer::KeywordStyleOf(const char* word) const {
    if (word == nullptr || *word == '\0') {
        return udl::kStyleIdentifier;
    }
    for (int group = 0; group < udl::kKeywordGroupCount; ++group) {
        if (keyWordLists[group] != nullptr && keyWordLists[group]->InList(word)) {
            return udl::KeywordStyleOf(group);
        }
    }
    return udl::kStyleIdentifier;
}

void SCI_METHOD UdlLexer::Lex(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle,
                              Scintilla::IDocument* pAccess) {
    Accessor styler(pAccess, nullptr);
    StyleContext sc(startPos, static_cast<Sci_PositionU>(lengthDoc), initStyle, styler);

    ActiveString string;
    if (initStyle == udl::kStyleString) {
        // Scintilla 从文档中途开始着色时会带一个 initStyle 进来：那时得先弄清
        // 当前这个字符串是用哪对界定符开的头
        string = RecoverString(styler, static_cast<Sci_Position>(startPos));
    }

    for (; sc.More(); sc.Forward()) {
        switch (sc.state) {
            case udl::kStyleCommentLine:
                if (sc.atLineEnd) {
                    sc.SetState(udl::kStyleDefault);
                }
                break;
            case udl::kStyleCommentBlock:
                if (!definition_.blockCommentEnd.empty() &&
                    sc.Match(definition_.blockCommentEnd.c_str())) {
                    // 结束符本身也算注释，所以先吃掉再切回默认
                    sc.ForwardBytes(static_cast<Sci_Position>(definition_.blockCommentEnd.size()));
                    sc.SetState(udl::kStyleDefault);
                }
                break;
            case udl::kStyleString:
                if (!string.escape.empty() && sc.ch == string.escape[0]) {
                    // 转义：把被转义的字符一起吞进来（\" 不该结束字符串）
                    sc.Forward();
                } else if (!string.end.empty() && sc.Match(string.end.c_str())) {
                    sc.ForwardBytes(static_cast<Sci_Position>(string.end.size()));
                    sc.SetState(udl::kStyleDefault);
                    string = ActiveString{};
                } else if (sc.atLineEnd) {
                    // 字符串不跨行：撞到行尾就收尾，免得把后面整篇都当成字符串
                    sc.SetState(udl::kStyleDefault);
                    string = ActiveString{};
                }
                break;
            case udl::kStyleNumber:
                // 数字体里允许字母：0x1F、1.5e3、12u 都能跟下来
                if (!IsWordBodyChar(sc.ch) && sc.ch != '.') {
                    sc.SetState(udl::kStyleDefault);
                }
                break;
            case udl::kStyleIdentifier:
                if (!IsWordBodyChar(sc.ch)) {
                    char buffer[kWordBufferSize] = {};
                    if (definition_.caseSensitive) {
                        sc.GetCurrent(buffer, sizeof(buffer));
                    } else {
                        // 词表也是按小写存的（WordListSet），所以这里取小写形式去比
                        sc.GetCurrentLowered(buffer, sizeof(buffer));
                    }
                    sc.ChangeState(KeywordStyleOf(buffer));
                    sc.SetState(udl::kStyleDefault);
                }
                break;
            case udl::kStyleOperator:
                if (sc.ch >= 0x80 ||
                    !IsOperatorChar(definition_.operators,
                                    static_cast<unsigned char>(sc.ch))) {
                    sc.SetState(udl::kStyleDefault);
                }
                break;
            default:
                break;
        }

        if (sc.state == udl::kStyleDefault) {
            // 注释与字符串的判定放在数字/标识符之前：// 里的 1 不该是数字
            if (!definition_.blockCommentStart.empty() &&
                sc.Match(definition_.blockCommentStart.c_str())) {
                sc.SetState(udl::kStyleCommentBlock);
            } else if (!definition_.lineComment.empty() &&
                       sc.Match(definition_.lineComment.c_str()) && AtTokenBoundary(sc)) {
                sc.SetState(udl::kStyleCommentLine);
            } else if (StartString(styler, static_cast<Sci_Position>(sc.currentPos), string)) {
                sc.SetState(udl::kStyleString);
            } else if (IsAsciiDigit(static_cast<char>(sc.ch)) ||
                       (sc.ch == '.' && IsAsciiDigit(static_cast<char>(sc.chNext)))) {
                sc.SetState(udl::kStyleNumber);
            } else if (IsWordStartChar(sc.ch)) {
                sc.SetState(udl::kStyleIdentifier);
            } else if (sc.ch < 0x80 &&
                       IsOperatorChar(definition_.operators, static_cast<unsigned char>(sc.ch))) {
                sc.SetState(udl::kStyleOperator);
            }
        }
    }
    sc.Complete();
}

void SCI_METHOD UdlLexer::Fold(Sci_PositionU startPos, Sci_Position lengthDoc, int /*initStyle*/,
                               Scintilla::IDocument* pAccess) {
    if (!definition_.foldable) {
        return;
    }
    Accessor styler(pAccess, nullptr);

    const Sci_Position lineDocEnd = static_cast<Sci_Position>(styler.GetLine(styler.Length()));
    Sci_Position lineCurrent = static_cast<Sci_Position>(styler.GetLine(startPos));
    int levelCurrent = SC_FOLDLEVELBASE;
    if (lineCurrent > 0) {
        levelCurrent = styler.LevelAt(lineCurrent - 1) & SC_FOLDLEVELNUMBERMASK;
    }
    const Sci_Position endPos = static_cast<Sci_Position>(startPos) + lengthDoc;

    while (lineCurrent <= lineDocEnd && styler.LineStart(lineCurrent) < endPos) {
        const Sci_Position lineStart = styler.LineStart(lineCurrent);
        const Sci_Position lineEnd = styler.LineEnd(lineCurrent);
        int opens = 0;
        int closes = 0;
        Sci_Position pos = lineStart;
        while (pos < lineEnd) {
            const int style = styler.StyleAt(pos);
            const bool code = style != udl::kStyleCommentLine &&
                              style != udl::kStyleCommentBlock &&
                              style != udl::kStyleString;   // 注释与字符串里的词不算折叠词
            const unsigned char byte = static_cast<unsigned char>(styler.SafeGetCharAt(pos, '\0'));
            if (!code || !IsWordStart(byte)) {
                ++pos;
                continue;
            }
            std::string word;
            while (pos < lineEnd) {
                const unsigned char body =
                    static_cast<unsigned char>(styler.SafeGetCharAt(pos, '\0'));
                if (!IsWordBody(body)) {
                    break;
                }
                word.push_back(definition_.caseSensitive
                                   ? static_cast<char>(body)
                                   : static_cast<char>(std::tolower(body)));
                ++pos;
            }
            const auto matches = [&](const std::vector<std::string>& tokens) {
                return std::find(tokens.begin(), tokens.end(), word) != tokens.end();
            };
            if (matches(definition_.foldOpenTokens)) {
                ++opens;
            } else if (matches(definition_.foldCloseTokens)) {
                ++closes;
            }
        }

        int levelNext = levelCurrent + opens - closes;
        if (levelNext < 0) {
            levelNext = 0;   // 收起词比打开词多时不要出现负数层级
        }
        int levelLine = levelCurrent;
        if (opens > closes) {
            // 这一行开了块：它自己是折叠头
            levelLine |= SC_FOLDLEVELHEADERFLAG;
        }
        styler.SetLevel(lineCurrent, levelLine);
        levelCurrent = levelNext;
        ++lineCurrent;
    }

    // 紧随其后的那一行也要有层级：否则折叠/展开时算出来的子行范围会错
    if (lineCurrent <= lineDocEnd) {
        styler.SetLevel(lineCurrent, levelCurrent);
    }
}

}  // namespace

Scintilla::ILexer5* CreateUdlLexer(const UdlDefinition& definition) {
    return new UdlLexer(definition);
}

}  // namespace npcc::edit
