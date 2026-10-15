#include "edit/EditView.h"

#include <windows.h>

#include <Scintilla.h>
// Lexilla.h 依赖 ILexer.h 中的 ILexer5（它自己不包含该头文件），必须先引入
#include <ILexer.h>
#include <ILoader.h>
#include <Lexilla.h>

#include <cwctype>
#include <string>
#include <vector>

// 把 EditView 对外暴露的查找标志钉死在 Scintilla 的取值上：
// 将来升级 Scintilla 若改了数值，编译期就会报错，而不是静默搜错。
static_assert(npcc::edit::EditView::kSearchFlagWholeWord == SCFIND_WHOLEWORD,
              "kSearchFlagWholeWord 必须与 SCFIND_WHOLEWORD 一致");
static_assert(npcc::edit::EditView::kSearchFlagMatchCase == SCFIND_MATCHCASE,
              "kSearchFlagMatchCase 必须与 SCFIND_MATCHCASE 一致");
static_assert(npcc::edit::EditView::kSelectionStream == SC_SEL_STREAM,
              "kSelectionStream 必须与 SC_SEL_STREAM 一致");
static_assert(npcc::edit::EditView::kSelectionRectangle == SC_SEL_RECTANGLE,
              "kSelectionRectangle 必须与 SC_SEL_RECTANGLE 一致");
static_assert(npcc::edit::EditView::kVirtualSpaceRectangle == SCVS_RECTANGULARSELECTION,
              "kVirtualSpaceRectangle 必须与 SCVS_RECTANGULARSELECTION 一致");
static_assert(npcc::edit::EditView::kRectModifierAlt == SCMOD_ALT,
              "kRectModifierAlt 必须与 SCMOD_ALT 一致");

namespace npcc::edit {
namespace {

// M0 的最小语言表：扩展名 -> (Lexilla lexer 名, 关键词组 0)。
// M1 起由 lang/LexerRegistry 从 langs.xml 载入，这里只是让 M0 有可见的语法着色。
struct BuiltinLanguage {
    const wchar_t* extension;
    const char*    lexer;
    const char*    keywords;
    const wchar_t* displayName;
    // 该 lexer 是否支持折叠。不支持的语言把折叠边距藏起来，
    // 否则会留一条永远空着的窄边距，看起来像坏了。
    bool           foldable;
};

constexpr const char* kCppKeywords =
    "alignas alignof auto bool break case catch char class const constexpr continue default "
    "delete do double else enum explicit extern false float for friend goto if inline int long "
    "mutable namespace new noexcept nullptr operator private protected public register return "
    "short signed sizeof static struct switch template this thread_local throw true try typedef "
    "typename union unsigned using virtual void volatile while wchar_t";

constexpr const char* kJsKeywords =
    "async await break case catch class const continue debugger default delete do else export "
    "extends false finally for function if import in instanceof let new null return super switch "
    "this throw true try typeof undefined var void while with yield";

constexpr const char* kTypeScriptKeywords =
    "abstract any as async await boolean break case catch class const constructor continue "
    "declare default delete do else enum export extends false finally for from function if "
    "implements import in instanceof interface let module namespace new null number object "
    "private protected public readonly return static string super switch this throw true try "
    "type typeof undefined var void while with yield";

const BuiltinLanguage kBuiltinLanguages[] = {
    // 同一族语言共用一份关键字表：写成 nullptr 的话关键字根本不会被着色
    // （词法器只认 SCI_SETKEYWORDS 给的那几组词），.h 与 .ts 就是这么被漏掉的。
    {L"cpp", "cpp", kCppKeywords, L"C/C++", true},
    {L"c",   "cpp", kCppKeywords, L"C/C++", true},
    {L"h",   "cpp", kCppKeywords, L"C/C++", true},
    {L"hpp", "cpp", kCppKeywords, L"C/C++", true},
    {L"py",  "python",
     "and as assert async await break class continue def del elif else except False finally for "
     "from global if import in is lambda None nonlocal not or pass raise return True try while "
     "with yield",
     L"Python", true},
    // 注意：Lexilla 里**没有** "javascript" 这个词法器（本版实测 CreateLexer 返回空，
    // 结果是 JS 文件连注释都不着色、也没法折叠）。业界通行做法是用 cpp 词法器
    // 配一套 JavaScript 关键字，注释/字符串/数字/花括号折叠都能正常工作。
    {L"js",  "cpp", kJsKeywords, L"JavaScript", true},
    {L"mjs", "cpp", kJsKeywords, L"JavaScript", true},
    {L"jsx", "cpp", kJsKeywords, L"JavaScript (JSX)", true},
    {L"ts",  "cpp", kTypeScriptKeywords, L"TypeScript", true},
    {L"json","json", nullptr, L"JSON", true},
    {L"xml", "xml", nullptr, L"XML", true},
    {L"html","hypertext", nullptr, L"HTML", true},
    {L"htm", "hypertext", nullptr, L"HTML", true},
    {L"css", "css", nullptr, L"CSS", true},
    {L"md",  "markdown", nullptr, L"Markdown", true},
    {L"sql", "sql",
     "select from where insert into values update set delete create table database index view "
     "drop alter join left right inner outer on group by order having union all distinct as",
     L"SQL", true},
    // batch / props 这两个 lexer 不算折叠层级，边距藏起来
    {L"bat", "batch", nullptr, L"Batch", false},
    {L"cmd", "batch", nullptr, L"Batch", false},
    {L"ps1", "powershell", nullptr, L"PowerShell", true},
    {L"ini", "props", nullptr, L"配置文件", false},
    {L"log", "null", nullptr, L"日志", false},
};

// ---------------------------------------------------------------- 语法着色的默认配色
//
// 光接上词法器不等于"用户看得见颜色"：词法器只负责把每个 token 标上样式号，
// 颜色得我们按样式号设进去。这些样式号取自 Lexilla 的 SCE_* 定义（third_party/
// lexilla/include/SciLexer.h），**不靠记忆写数字**——写错的表现是"某个语言没颜色"，
// 而且不报任何错。单元测试会按真实 token 位置核对颜色确实生效。
//
// M2 的样式配置器会在这张表之上做用户可调，所以这里只管"开箱可用"。
struct SyntaxStyleMap {
    const char* lexer;
    int commentLine;    // -1 表示该语言没有这一类
    int commentBlock;
    int keyword;
    int number;
    int string;
    int highlight;      // 语言特有的强调项：预处理、变量、属性名、键…
    int extra;
};

constexpr int kCommentColour = 0x2E7D32;    // 注释：绿
constexpr int kKeywordColour = 0x0000C0;    // 关键字：深蓝
constexpr int kNumberColour = 0x986500;     // 数字：棕
constexpr int kStringColour = 0xA31515;     // 字符串：暗红
constexpr int kHighlightColour = 0x800080;  // 强调项：紫
constexpr int kExtraColour = 0x266B9E;      // 次要项：蓝

// 样式号对应关系（Lexilla 的 SCE_*）：
//   cpp       : COMMENTLINE=2 COMMENT=1 WORD=5 NUMBER=4 STRING=6 PREPROCESSOR=9 CHARACTER=7
//   python    : COMMENTLINE=1 WORD=5 NUMBER=2 STRING=3
//   json      : LINECOMMENT=6 BLOCKCOMMENT=7 NUMBER=1 STRING=2 PROPERTYNAME=4
//   hypertext : COMMENT=9 TAG=1 NUMBER=5 DOUBLESTRING=6 ATTRIBUTE=3 ENTITY=10
//   css       : COMMENT=9 TAG=1 CLASS=2 VALUE=8
//   markdown  : HEADER1=6 STRONG1=2 CODE=19 LINK=18
//   sql       : COMMENTLINE=2 COMMENT=1 WORD=5 NUMBER=4 CHARACTER=7
//   powershell: COMMENT=1 COMMENTSTREAM=13 KEYWORD=8 NUMBER=4 STRING=2 VARIABLE=5 CMDLET=9
//   batch     : COMMENT=1 WORD=2
//   props     : COMMENT=1 SECTION=2 KEY=5 DEFVAL=4
const SyntaxStyleMap kSyntaxMaps[] = {
    {"cpp", 2, 1, 5, 4, 6, 9, 7},
    {"python", 1, -1, 5, 2, 3, -1, -1},
    {"json", 6, 7, -1, 1, 2, 4, -1},
    {"xml", 9, -1, 1, 5, 6, 3, 10},
    {"hypertext", 9, -1, 1, 5, 6, 3, 10},
    {"css", 9, -1, 1, -1, -1, 2, 8},
    {"markdown", -1, -1, 6, -1, 19, 2, 18},
    {"sql", 2, 1, 5, 4, 7, -1, -1},
    {"powershell", 1, 13, 8, 4, 2, 5, 9},
    {"batch", 1, -1, 2, -1, -1, -1, -1},
    {"props", 1, -1, 2, -1, -1, 5, 4},
};

const SyntaxStyleMap* FindSyntaxMap(const char* lexerName) {
    if (lexerName == nullptr) {
        return nullptr;
    }
    for (const SyntaxStyleMap& entry : kSyntaxMaps) {
        if (std::string_view(entry.lexer) == lexerName) {
            return &entry;
        }
    }
    return nullptr;
}

const BuiltinLanguage* FindLanguage(const std::wstring& extension, const BuiltinLanguage* fallback) {
    for (const auto& entry : kBuiltinLanguages) {
        if (extension == entry.extension) {
            return &entry;
        }
    }
    return fallback;
}

std::wstring ExtensionOf(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) {
        return {};
    }
    std::wstring extension = path.substr(dot + 1);
    for (wchar_t& ch : extension) {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return extension;
}

}  // namespace

EditView::~EditView() {
    Destroy();
}

bool RegisterEditorClasses(void* instance) {
    return Scintilla_RegisterClasses(instance) != 0;
}

bool EditView::Create(HWND parent, int controlId) {
    if (hwnd_ != nullptr) {
        return true;
    }
    hwnd_ = ::CreateWindowExW(
        0, L"Scintilla", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        ::GetModuleHandleW(nullptr), nullptr);
    return hwnd_ != nullptr;
}

void EditView::Destroy() {
    if (hwnd_ != nullptr) {
        if (::IsWindow(hwnd_) != 0) {
            ::DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }
}

void EditView::SetVisible(bool visible) const {
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
}

void EditView::Resize(const RECT& client) const {
    if (hwnd_ != nullptr) {
        ::MoveWindow(hwnd_, 0, 0, client.right - client.left, client.bottom - client.top, TRUE);
    }
}

intptr_t EditView::Send(uint32_t message, uintptr_t wParam, intptr_t lParam) const {
    if (hwnd_ == nullptr) {
        return 0;
    }
    return static_cast<intptr_t>(
        ::SendMessageW(hwnd_, message, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)));
}

bool EditView::TranslateNotify(const void* nmhdr, EditorNotification& out) {
    if (nmhdr == nullptr) {
        return false;
    }
    const auto* header = static_cast<const NMHDR*>(nmhdr);
    if (header->code < SCN_STYLENEEDED || header->code > SCN_AUTOCSELECTIONCHANGE) {
        return false;
    }
    const auto* notification = static_cast<const SCNotification*>(nmhdr);
    out.event = static_cast<EditorEvent>(header->code);
    out.position = static_cast<int32_t>(notification->position);
    out.modificationType = notification->modificationType;
    out.updated = notification->updated;
    // SCNotification::line 是 Sci_Position（64 位），显式收窄避免 /W4 的 C4244
    out.line = static_cast<int32_t>(notification->line);
    out.margin = notification->margin;
    return true;
}

void EditView::SetCodePage(uint32_t codePage) const {
    Send(SCI_SETCODEPAGE, codePage);
}

void EditView::SetEolMode(EolMode eol) const {
    int mode = SC_EOL_CRLF;
    switch (eol) {
        case EolMode::Lf: mode = SC_EOL_LF; break;
        case EolMode::Cr: mode = SC_EOL_CR; break;
        default:          mode = SC_EOL_CRLF; break;
    }
    Send(SCI_SETEOLMODE, static_cast<uintptr_t>(mode));
}

void EditView::SetText(std::string_view bytes) const {
    // SCI_SETTEXT 要求 NUL 结尾，string_view 不保证，必须复制一份
    const std::string owned(bytes);
    Send(SCI_SETTEXT, 0, reinterpret_cast<intptr_t>(owned.c_str()));
}

std::string EditView::GetAllText() const {
    const intptr_t length = Send(SCI_GETLENGTH);
    if (length <= 0) {
        return {};
    }
    // 缓冲区要留出结尾 NUL，取回后再截掉，否则会越界写一个字节
    std::string buffer(static_cast<size_t>(length) + 1, '\0');
    Send(SCI_GETTEXT, static_cast<uintptr_t>(length) + 1,
         reinterpret_cast<intptr_t>(buffer.data()));
    buffer.resize(static_cast<size_t>(length));
    return buffer;
}

bool EditView::LoadFromChunkedData(const char* data, std::uint64_t size,
                                   bool largeDocument) const {
    if (hwnd_ == nullptr || data == nullptr) {
        return false;
    }
    // DocumentOption::TextLarge = 0x100，提示 Scintilla 按大文档处理
    const intptr_t option = largeDocument ? 0x100 : 0;
    auto* loader = reinterpret_cast<Scintilla::ILoader*>(
        Send(SCI_CREATELOADER, static_cast<uintptr_t>(size), option));
    if (loader == nullptr) {
        return false;
    }

    constexpr std::uint64_t kChunkBytes = 1ull << 20;   // 每次投喂 1 MB
    std::uint64_t offset = 0;
    int status = 0;
    while (offset < size) {
        const std::uint64_t remaining = size - offset;
        const auto length = static_cast<Sci_Position>(
            remaining < kChunkBytes ? remaining : kChunkBytes);
        status = loader->AddData(data + offset, length);
        if (status != 0) {   // SC_STATUS_OK == 0
            break;
        }
        offset += static_cast<std::uint64_t>(length);
    }
    if (status != 0) {
        loader->Release();
        return false;
    }

    if (void* document = loader->ConvertToDocument(); document != nullptr) {
        // SCI_SETDOCPOINTER 会自己 AddRef，调用方的引用由 Release 归还
        Send(SCI_SETDOCPOINTER, 0, reinterpret_cast<intptr_t>(document));
    }
    loader->Release();
    return true;
}

void EditView::ApplyLargeFileDisplayMode() const {
    // 大文件档连折叠一起关掉：算折叠层级是 lexer 扫全文的活，正是这一档要省的东西
    largeFileMode_ = true;
    SetFoldingEnabled(false);
    SetWrapMode(false);
    SetShowWhitespace(false);
    SetShowEol(false);
    Send(SCI_SETINDENTATIONGUIDES, 0);
    Send(SCI_SETCARETLINEVISIBLE, 0);
}

void EditView::ApplyHugeFileMode() const {
    ApplyLargeFileDisplayMode();
    SetReadOnly(true);
    Send(SCI_SETUNDOCOLLECTION, 0);
    Send(SCI_SETMARGINWIDTHN, static_cast<uintptr_t>(kMarginBookmark), 0);
    // 超大文件不做词法分析：Lexilla 会扫描全文，代价太高
    Scintilla::ILexer5* plain = CreateLexer("null");
    Send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(plain));
    languageName_ = L"纯文本（大文件）";
}

void EditView::SetReadOnly(bool readOnly) const {
    Send(SCI_SETREADONLY, readOnly ? 1u : 0u);
}

bool EditView::IsModified() const {
    return Send(SCI_GETMODIFY) != 0;
}

void EditView::MarkSavePoint() const {
    Send(SCI_SETSAVEPOINT);
}

void EditView::ClearUndoBuffer() const {
    Send(SCI_EMPTYUNDOBUFFER);
}

void EditView::SetDefaultFont(const char* faceName, int pointSize) const {
    Send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<intptr_t>(faceName));
    Send(SCI_STYLESETSIZE, STYLE_DEFAULT, pointSize);
    Send(SCI_STYLECLEARALL);   // 把默认样式扩散到所有样式
}

void EditView::ApplyLightTheme() const {
    Send(SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0x1E, 0x1E, 0x1E));
    Send(SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0xFF, 0xFF, 0xFF));
    SetDefaultFont("Consolas", 11);

    // 行号边距
    Send(SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x80, 0x80, 0x80));
    Send(SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(0xF5, 0xF5, 0xF5));
    Send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    Send(SCI_SETMARGINWIDTHN, 0, 48);
    ConfigureBookmarkMargin();
    ConfigureFoldMargin();

    Send(SCI_SETCARETFORE, RGB(0x00, 0x00, 0x00));
    Send(SCI_SETCARETLINEVISIBLE, 1);
    Send(SCI_SETCARETLINEBACK, RGB(0xF2, 0xF6, 0xFA));
    Send(SCI_SETSELBACK, 1, RGB(0xCC, 0xE0, 0xF5));
    SetTabWidth(4, false);
}

void EditView::ShowLineNumbers(bool visible) const {
    Send(SCI_SETMARGINWIDTHN, 0, visible ? 48 : 0);
}

void EditView::ConfigureBookmarkMargin() const {
    // 书签放在独立边距里：可以"在行号右侧、可点击"，而不必去动行号边距
    Send(SCI_MARKERDEFINE, static_cast<uintptr_t>(kMarkerBookmark), SC_MARK_CIRCLE);
    Send(SCI_MARKERSETFORE, static_cast<uintptr_t>(kMarkerBookmark), RGB(0xFF, 0xFF, 0xFF));
    Send(SCI_MARKERSETBACK, static_cast<uintptr_t>(kMarkerBookmark), RGB(0x2E, 0x86, 0xC1));
    Send(SCI_SETMARGINTYPEN, static_cast<uintptr_t>(kMarginBookmark), SC_MARGIN_SYMBOL);
    Send(SCI_SETMARGINMASKN, static_cast<uintptr_t>(kMarginBookmark), 1u << kMarkerBookmark);
    Send(SCI_SETMARGINWIDTHN, static_cast<uintptr_t>(kMarginBookmark), 16);
    Send(SCI_SETMARGINSENSITIVEN, static_cast<uintptr_t>(kMarginBookmark), 1);
}

void EditView::ConfigureFoldMargin() const {
    // 折叠符号用 Scintilla 预留的 marker 号（SC_MARKNUM_FOLDER*，25~31）。
    // 这七个 marker 各有分工：折叠头 / 展开头 / 竖线 / 收尾，只画其中两个，
    // 其余定义成 EMPTY，免得默认样式在缩进层级上留下一串莫名其妙的方框。
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_PLUS);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_MINUS);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_EMPTY);
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDER, RGB(0x60, 0x60, 0x60));
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDER, RGB(0xFF, 0xFF, 0xFF));
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDEROPEN, RGB(0x60, 0x60, 0x60));
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDEROPEN, RGB(0xFF, 0xFF, 0xFF));

    Send(SCI_SETMARGINTYPEN, static_cast<uintptr_t>(kMarginFold), SC_MARGIN_SYMBOL);
    Send(SCI_SETMARGINMASKN, static_cast<uintptr_t>(kMarginFold), SC_MASK_FOLDERS);
    Send(SCI_SETMARGINSENSITIVEN, static_cast<uintptr_t>(kMarginFold), 1);
    // 折叠后在头行下面画一条横线，视觉上能看出"这里被折起来了"
    Send(SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED);
    // 交给 Scintilla 自己管：边距点击切换、编辑时重算、算完就显示符号。
    // 这三件事都由它做，比我们自己在 WM_NOTIFY 里拼要可靠（也不会漏掉重算时机）。
    Send(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK |
                                   SC_AUTOMATICFOLD_CHANGE);
    Send(SCI_SETMARGINWIDTHN, static_cast<uintptr_t>(kMarginFold), 0);   // 由语言决定是否显示
    foldingEnabled_ = false;
}

void EditView::SetFoldingEnabled(bool enabled) const {
    // 大文件档一律不给折叠：那一档要省的正是"lexer 扫全文"
    foldingEnabled_ = enabled && !largeFileMode_;
    Send(SCI_SETMARGINWIDTHN, static_cast<uintptr_t>(kMarginFold), foldingEnabled_ ? 14 : 0);
    if (!foldingEnabled_) {
        UnfoldAll();   // 关掉折叠时把已折的行展开，免得内容"消失"又没有边距可以点开
    }
}

bool EditView::FoldingEnabled() const {
    return foldingEnabled_;
}

int32_t EditView::FoldLevelOf(int32_t line) const {
    if (line < 0) {
        return 0;
    }
    return static_cast<int32_t>(Send(SCI_GETFOLDLEVEL, static_cast<uintptr_t>(line)));
}

bool EditView::LineIsFoldHeader(int32_t line) const {
    return line >= 0 && (FoldLevelOf(line) & SC_FOLDLEVELHEADERFLAG) != 0;
}

bool EditView::IsLineFolded(int32_t line) const {
    return line >= 0 && Send(SCI_GETFOLDEXPANDED, static_cast<uintptr_t>(line)) == 0;
}

void EditView::ToggleFoldAt(int32_t line) const {
    if (LineIsFoldHeader(line)) {
        Send(SCI_TOGGLEFOLD, static_cast<uintptr_t>(line));
    }
}

void EditView::ToggleFoldAtCaret() const {
    // 光标在块内部时，往上找最近的一个折叠头——那正是包住光标这一层。
    // 不这样做的话，用户在函数体里按折叠键会毫无反应，而他期待的明显是
    // "把这一层折起来"。
    int32_t target = CurrentLine();
    const int32_t lowest = 0;
    while (target > lowest && !LineIsFoldHeader(target)) {
        --target;
    }
    ToggleFoldAt(target);
}

void EditView::FoldAll() const {
    Send(SCI_FOLDALL, SC_FOLDACTION_CONTRACT);
}

void EditView::UnfoldAll() const {
    Send(SCI_FOLDALL, SC_FOLDACTION_EXPAND);
}

int32_t EditView::VisibleLineCount() const {
    const int32_t lines = LineCount();
    if (lines <= 0) {
        return 0;
    }
    return static_cast<int32_t>(
               Send(SCI_VISIBLEFROMDOCLINE, static_cast<uintptr_t>(lines - 1))) + 1;
}

std::vector<int32_t> EditView::FoldedLines() const {
    std::vector<int32_t> folded;
    if (!foldingEnabled_) {
        return folded;
    }
    // 上限是防手滑：一个文件真被折成几万处，会话文件会大得没意义
    constexpr std::size_t kMaxFoldedLines = 4096;
    const int32_t lines = LineCount();
    for (int32_t line = 0; line < lines && folded.size() < kMaxFoldedLines; ++line) {
        if (LineIsFoldHeader(line) && IsLineFolded(line)) {
            folded.push_back(line);
        }
    }
    return folded;
}

void EditView::RestoreFoldedLines(const std::vector<int32_t>& lines) const {
    if (!foldingEnabled_) {
        return;
    }
    const int32_t count = LineCount();
    for (const int32_t line : lines) {
        if (line >= 0 && line < count && LineIsFoldHeader(line)) {
            // 注意第二个参数是"折叠动作"（CONTRACT/EXPAND/TOGGLE），不是 fold flags。
            // 老版本的文档写的是 flags，照抄会传进一个非法值然后静默什么都不做——
            // 单元测试就是在这里抓到它的（恢复折叠一直没生效，也没有任何报错）。
            Send(SCI_FOLDLINE, static_cast<uintptr_t>(line), SC_FOLDACTION_CONTRACT);
        }
    }
}

bool EditView::ShareDocumentWith(const EditView& source) {
    if (hwnd_ == nullptr || source.hwnd_ == nullptr || hwnd_ == source.hwnd_) {
        return false;
    }
    const intptr_t document = source.Send(SCI_GETDOCPOINTER);
    if (document == 0) {
        return false;
    }
    // SETDOCPOINTER 自己会给新文档加一次引用、并释放本视图原先持有的那份，
    // 所以这里不需要（也不应该）手工调 ADDREFDOCUMENT/RELEASEDOCUMENT。
    Send(SCI_SETDOCPOINTER, 0, document);
    return true;
}

void EditView::ConfigureColumnEditing() const {
    // 默认情况下 Scintilla 不允许鼠标切到矩形选择，必须显式打开
    Send(SCI_SETMOUSESELECTIONRECTANGULARSWITCH, 1);
    Send(SCI_SETRECTANGULARSELECTIONMODIFIER,
         static_cast<uintptr_t>(kRectModifierAlt));
    // 允许光标在矩形模式下越过行尾，否则在参差不齐的行上没法选同一列
    Send(SCI_SETVIRTUALSPACEOPTIONS,
         static_cast<uintptr_t>(kVirtualSpaceRectangle));
}

bool EditView::IsRectangularSelection() const {
    return Send(SCI_SELECTIONISRECTANGLE) != 0;
}

void EditView::SetSelectionMode(int32_t mode) const {
    Send(SCI_SETSELECTIONMODE, static_cast<uintptr_t>(mode));
}

int32_t EditView::SelectionMode() const {
    return static_cast<int32_t>(Send(SCI_GETSELECTIONMODE));
}

void EditView::SetRectangularSelection(int32_t anchorPos, int32_t caretPos) const {
    Send(SCI_SETSELECTIONMODE, static_cast<uintptr_t>(kSelectionRectangle));
    Send(SCI_SETRECTANGULARSELECTIONANCHOR, static_cast<uintptr_t>(anchorPos));
    Send(SCI_SETRECTANGULARSELECTIONCARET, static_cast<uintptr_t>(caretPos));
}

void EditView::ColumnRange(int32_t& firstLine, int32_t& lastLine) const {
    firstLine = static_cast<int32_t>(
        Send(SCI_LINEFROMPOSITION, static_cast<uintptr_t>(SelectionStart())));
    lastLine = static_cast<int32_t>(
        Send(SCI_LINEFROMPOSITION, static_cast<uintptr_t>(SelectionEnd())));
    if (lastLine < firstLine) {
        std::swap(firstLine, lastLine);
    }
}

int32_t EditView::LineSelectionStart(int32_t line) const {
    return static_cast<int32_t>(Send(SCI_GETLINESELSTARTPOSITION,
                                     static_cast<uintptr_t>(line)));
}

int32_t EditView::LineSelectionEnd(int32_t line) const {
    return static_cast<int32_t>(Send(SCI_GETLINESELENDPOSITION,
                                     static_cast<uintptr_t>(line)));
}

void EditView::InsertTextAt(int32_t position, std::string_view text) const {
    if (text.empty() || position < 0) {
        return;
    }
    const std::string owned(text);   // SCI_INSERTTEXT 要 NUL 结尾
    Send(SCI_INSERTTEXT, static_cast<uintptr_t>(position),
         reinterpret_cast<intptr_t>(owned.c_str()));
}

void EditView::DeleteRangeAt(int32_t start, int32_t end) const {
    if (end <= start) {
        return;
    }
    Send(SCI_DELETERANGE, static_cast<uintptr_t>(start),
         static_cast<intptr_t>(end - start));
}

bool EditView::OverwriteMode() const {
    return Send(SCI_GETOVERTYPE) != 0;
}

void EditView::SetOverwriteMode(bool enabled) const {
    Send(SCI_SETOVERTYPE, enabled ? 1u : 0u);
}

void EditView::SetUndoCollection(bool enabled) const {
    Send(SCI_SETUNDOCOLLECTION, enabled ? 1u : 0u);
}

int32_t EditView::ByteLength() const {
    const intptr_t length = Send(SCI_GETLENGTH);
    return length > 0 ? static_cast<int32_t>(length) : 0;
}

int32_t EditView::CopyTextTo(char* buffer, int32_t capacity) const {
    if (buffer == nullptr || capacity <= 0) {
        return 0;
    }
    buffer[0] = '\0';
    const intptr_t copied = Send(SCI_GETTEXT, static_cast<uintptr_t>(capacity),
                                 reinterpret_cast<intptr_t>(buffer));
    return copied > 0 ? static_cast<int32_t>(copied) : 0;
}

int32_t EditView::StyleAt(int32_t position) const {
    if (position < 0 || position >= ByteLength()) {
        return 0;
    }
    return static_cast<int32_t>(Send(SCI_GETSTYLEAT, static_cast<uintptr_t>(position)));
}

int32_t EditView::StyleForeColor(int32_t style) const {
    if (style < 0) {
        return -1;
    }
    return static_cast<int32_t>(
        Send(SCI_STYLEGETFORE, static_cast<uintptr_t>(style)));
}

int32_t EditView::DefaultForeColor() const {
    return static_cast<int32_t>(Send(SCI_STYLEGETFORE, static_cast<uintptr_t>(STYLE_DEFAULT)));
}

bool EditView::HasBookmark(int32_t line) const {
    const intptr_t mask = Send(SCI_MARKERGET, static_cast<uintptr_t>(line));
    return (mask & (1 << kMarkerBookmark)) != 0;
}

void EditView::AddBookmark(int32_t line) const {
    if (!HasBookmark(line)) {
        Send(SCI_MARKERADD, static_cast<uintptr_t>(line), kMarkerBookmark);
    }
}

void EditView::RemoveBookmark(int32_t line) const {
    Send(SCI_MARKERDELETE, static_cast<uintptr_t>(line), kMarkerBookmark);
}

void EditView::ToggleBookmark(int32_t line) const {
    if (HasBookmark(line)) {
        RemoveBookmark(line);
    } else {
        AddBookmark(line);
    }
}

void EditView::ClearAllBookmarks() const {
    Send(SCI_MARKERDELETEALL, static_cast<uintptr_t>(kMarkerBookmark));
}

int32_t EditView::NextBookmarkLine(int32_t fromLine, bool wrap) const {
    const intptr_t mask = 1 << kMarkerBookmark;
    if (fromLine < 0) {
        fromLine = 0;
    }
    const intptr_t found = Send(SCI_MARKERNEXT, static_cast<uintptr_t>(fromLine),
                                static_cast<intptr_t>(mask));
    if (found >= 0) {
        return static_cast<int32_t>(found);
    }
    if (!wrap) {
        return -1;
    }
    const intptr_t wrapped = Send(SCI_MARKERNEXT, 0, static_cast<intptr_t>(mask));
    return wrapped >= 0 ? static_cast<int32_t>(wrapped) : -1;
}

int32_t EditView::PrevBookmarkLine(int32_t fromLine, bool wrap) const {
    const intptr_t mask = 1 << kMarkerBookmark;
    const intptr_t found = Send(SCI_MARKERPREVIOUS, static_cast<uintptr_t>(fromLine),
                                static_cast<intptr_t>(mask));
    if (found >= 0) {
        return static_cast<int32_t>(found);
    }
    if (!wrap) {
        return -1;
    }
    // 回绕到最后一个书签：从末行往前找
    const intptr_t wrapped = Send(SCI_MARKERPREVIOUS,
                                  static_cast<uintptr_t>(LineCount() - 1),
                                  static_cast<intptr_t>(mask));
    return wrapped >= 0 ? static_cast<int32_t>(wrapped) : -1;
}

std::vector<int32_t> EditView::AllBookmarkLines() const {
    std::vector<int32_t> lines;
    const intptr_t mask = 1 << kMarkerBookmark;
    intptr_t line = Send(SCI_MARKERNEXT, 0, static_cast<intptr_t>(mask));
    while (line >= 0) {
        lines.push_back(static_cast<int32_t>(line));
        line = Send(SCI_MARKERNEXT, static_cast<uintptr_t>(line + 1),
                    static_cast<intptr_t>(mask));
    }
    return lines;
}

void EditView::SetShowWhitespace(bool enabled) const {
    Send(SCI_SETVIEWWS, enabled ? 1u : 0u);
    Send(SCI_SETINDENTATIONGUIDES, enabled ? 1u : 0u);
}

void EditView::SetShowEol(bool enabled) const {
    Send(SCI_SETVIEWEOL, enabled ? 1u : 0u);
}

void EditView::SetIndentGuides(bool enabled) const {
    Send(SCI_SETINDENTATIONGUIDES, enabled ? 1u : 0u);
}

void EditView::SetCaretLineVisible(bool enabled) const {
    Send(SCI_SETCARETLINEVISIBLE, enabled ? 1u : 0u);
}

void EditView::SetWrapMode(bool enabled) const {
    Send(SCI_SETWRAPMODE, enabled ? 1u : 0u);
    Send(SCI_SETWRAPVISUALFLAGS, enabled ? 1u : 0u);
}

void EditView::SetZoom(int32_t deltaSteps) const {
    Send(SCI_SETZOOM, static_cast<uintptr_t>(static_cast<intptr_t>(deltaSteps)));
}

int32_t EditView::ZoomLevel() const {
    return static_cast<int32_t>(Send(SCI_GETZOOM));
}

void EditView::SetTabWidth(int32_t width, bool useTabs) const {
    Send(SCI_SETTABWIDTH, static_cast<uintptr_t>(width));
    Send(SCI_SETUSETABS, useTabs ? 1u : 0u);
}

void EditView::ApplySyntaxColours(const char* lexerName) const {
    const SyntaxStyleMap* map = FindSyntaxMap(lexerName);
    if (map == nullptr) {
        return;   // 没有配色表的语言用默认色，不强求
    }
    struct Entry {
        int  style;
        int  colour;
        bool bold;
        bool italic;
    };
    const Entry entries[] = {
        {map->commentLine, kCommentColour, false, true},
        {map->commentBlock, kCommentColour, false, true},
        {map->keyword, kKeywordColour, true, false},
        {map->number, kNumberColour, false, false},
        {map->string, kStringColour, false, false},
        {map->highlight, kHighlightColour, false, false},
        {map->extra, kExtraColour, false, false},
    };
    for (const Entry& entry : entries) {
        if (entry.style < 0) {
            continue;
        }
        Send(SCI_STYLESETFORE, static_cast<uintptr_t>(entry.style),
             static_cast<intptr_t>(entry.colour));
        if (entry.bold) {
            Send(SCI_STYLESETBOLD, static_cast<uintptr_t>(entry.style), 1);
        }
        if (entry.italic) {
            Send(SCI_STYLESETITALIC, static_cast<uintptr_t>(entry.style), 1);
        }
    }
}

void EditView::ApplyLanguageForPath(const std::wstring& filePath) const {
    const BuiltinLanguage* language = FindLanguage(ExtensionOf(filePath), nullptr);
    if (language == nullptr) {
        languageName_ = L"普通文本";
        Scintilla::ILexer5* plain = CreateLexer("null");
        Send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(plain));
        SetFoldingEnabled(false);
        return;
    }
    languageName_ = language->displayName;
    Scintilla::ILexer5* lexer = CreateLexer(language->lexer);
    Send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(lexer));
    if (language->keywords != nullptr) {
        Send(SCI_SETKEYWORDS, 0, reinterpret_cast<intptr_t>(language->keywords));
    }
    if (language->foldable) {
        // 折叠层级是 lexer 自己算的，但要不要算得先告诉它。
        // fold.compact=0 是关键：默认值会把 "}" 后面没有正文的块折起来，
        // 折出来的看起来像丢了一行。
        Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold"),
             reinterpret_cast<intptr_t>("1"));
        Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold.compact"),
             reinterpret_cast<intptr_t>("0"));
        Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold.comment"),
             reinterpret_cast<intptr_t>("1"));
        const std::string_view lexerName(language->lexer);
        if (lexerName == "cpp" || lexerName == "xml" || lexerName == "hypertext") {
            // 预处理块（#if / #region）也能折
            Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold.preprocessor"),
                 reinterpret_cast<intptr_t>("1"));
        }
    }
    SetFoldingEnabled(language->foldable);
    ApplySyntaxColours(language->lexer);
    // 语言切换后需要整体重新着色
    Send(SCI_COLOURISE, 0, -1);
}

int32_t EditView::SelectionStart() const {
    return static_cast<int32_t>(Send(SCI_GETSELECTIONSTART));
}

int32_t EditView::SelectionEnd() const {
    return static_cast<int32_t>(Send(SCI_GETSELECTIONEND));
}

int32_t EditView::CurrentLine() const {
    return static_cast<int32_t>(Send(SCI_LINEFROMPOSITION, static_cast<uintptr_t>(Send(SCI_GETCURRENTPOS)), 0));
}

int32_t EditView::CurrentColumn() const {
    return static_cast<int32_t>(Send(SCI_GETCOLUMN, static_cast<uintptr_t>(Send(SCI_GETCURRENTPOS)), 0));
}

int32_t EditView::LineCount() const {
    return static_cast<int32_t>(Send(SCI_GETLINECOUNT));
}

int32_t EditView::SelectionByteCount() const {
    const int32_t start = SelectionStart();
    const int32_t end = SelectionEnd();
    return end > start ? end - start : 0;
}

void EditView::SetCaretPosition(int32_t position) const {
    Send(SCI_GOTOPOS, static_cast<uintptr_t>(position));
}

void EditView::SetFirstVisibleLine(int32_t line) const {
    Send(SCI_SETFIRSTVISIBLELINE, static_cast<uintptr_t>(line));
}

int32_t EditView::FirstVisibleLine() const {
    return static_cast<int32_t>(Send(SCI_GETFIRSTVISIBLELINE));
}

int32_t EditView::CaretPosition() const {
    return static_cast<int32_t>(Send(SCI_GETCURRENTPOS));
}

void EditView::ScrollToLine(int32_t line) const {
    Send(SCI_GOTOLINE, static_cast<uintptr_t>(line));
}

int64_t EditView::PositionFromLine(int32_t line) const {
    return static_cast<int64_t>(Send(SCI_POSITIONFROMLINE, static_cast<uintptr_t>(line)));
}

void EditView::SetSearchFlags(int32_t flags) const {
    Send(SCI_SETSEARCHFLAGS, static_cast<uintptr_t>(flags));
}

void EditView::SetTarget(int64_t start, int64_t end) const {
    Send(SCI_SETTARGETSTART, static_cast<uintptr_t>(start));
    Send(SCI_SETTARGETEND, static_cast<uintptr_t>(end));
}

int64_t EditView::SearchInTarget(std::string_view bytes) const {
    if (bytes.empty()) {
        return -1;
    }
    const intptr_t position =
        Send(SCI_SEARCHINTARGET, static_cast<uintptr_t>(bytes.size()),
             reinterpret_cast<intptr_t>(bytes.data()));
    return static_cast<int64_t>(position);
}

void EditView::ReplaceSelection(std::string_view bytes) const {
    // SCI_REPLACESEL 要求 NUL 结尾
    const std::string owned(bytes);
    Send(SCI_REPLACESEL, 0, reinterpret_cast<intptr_t>(owned.c_str()));
}

void EditView::SetSelectionRange(int64_t start, int64_t end) const {
    Send(SCI_SETSEL, static_cast<uintptr_t>(start), static_cast<intptr_t>(end));
}

int64_t EditView::TextLength() const {
    return static_cast<int64_t>(Send(SCI_GETLENGTH));
}

std::string EditView::GetRange(int64_t start, int64_t end) const {
    if (end <= start) {
        return {};
    }
    std::string buffer(static_cast<size_t>(end - start), '\0');
    Sci_TextRangeFull range{};
    range.chrg.cpMin = static_cast<Sci_Position>(start);
    range.chrg.cpMax = static_cast<Sci_Position>(end);
    range.lpstrText = buffer.data();
    Send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<intptr_t>(&range));
    return buffer;
}

void EditView::BeginUndoAction() const {
    Send(SCI_BEGINUNDOACTION);
}

void EditView::EndUndoAction() const {
    Send(SCI_ENDUNDOACTION);
}

void EditView::EnsureLineVisible(int32_t line) const {
    Send(SCI_ENSUREVISIBLE, static_cast<uintptr_t>(line));
}

void EditView::ScrollCaretToVisible() const {
    Send(SCI_SCROLLCARET);
}

void EditView::ConfigureSearchIndicators() const {
    Send(SCI_INDICSETSTYLE, kIndicatorAllMatches, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, kIndicatorAllMatches, RGB(0x2E, 0x86, 0xC1));
    Send(SCI_INDICSETALPHA, kIndicatorAllMatches, 80);
    Send(SCI_INDICSETOUTLINEALPHA, kIndicatorAllMatches, 200);
}

void EditView::MarkRange(int indicatorId, int64_t start, int64_t end) const {
    Send(SCI_SETINDICATORCURRENT, static_cast<uintptr_t>(indicatorId));
    Send(SCI_INDICATORFILLRANGE, static_cast<uintptr_t>(start),
         static_cast<intptr_t>(end - start));
}

void EditView::ClearIndicator(int indicatorId) const {
    const int64_t length = TextLength();
    Send(SCI_SETINDICATORCURRENT, static_cast<uintptr_t>(indicatorId));
    Send(SCI_INDICATORCLEARRANGE, 0, static_cast<intptr_t>(length));
}

}  // namespace npcc::edit
