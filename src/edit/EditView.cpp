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

#include "edit/UdlLexer.h"
#include "edit/UdlStyles.h"
#include "edit/StyleCatalogue.h"
#include "infra/Log.h"

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
// 打印相关的取值同样钉死：SC_PRINT_* / SC_WRAP_* 改数值的话，编译期就报出来
static_assert(npcc::edit::EditView::kPrintNormal == SC_PRINT_NORMAL, "打印着色模式");
static_assert(npcc::edit::EditView::kPrintInvertLight == SC_PRINT_INVERTLIGHT, "打印着色模式");
static_assert(npcc::edit::EditView::kPrintBlackOnWhite == SC_PRINT_BLACKONWHITE, "打印着色模式");
static_assert(npcc::edit::EditView::kPrintColourOnWhite == SC_PRINT_COLOURONWHITE, "打印着色模式");
static_assert(npcc::edit::EditView::kPrintColourOnWhiteDefaultBg ==
                  SC_PRINT_COLOURONWHITEDEFAULTBG,
              "打印着色模式");
static_assert(npcc::edit::EditView::kPrintScreenColours == SC_PRINT_SCREENCOLOURS,
              "打印着色模式");
static_assert(npcc::edit::EditView::kPrintWrapNone == SC_WRAP_NONE, "打印换行模式");
static_assert(npcc::edit::EditView::kPrintWrapWord == SC_WRAP_WORD, "打印换行模式");

namespace npcc::edit {
namespace {

// 内置语言表的单例：单元测试与没传 registry 的调用点用它兜底。
// 应用侧走 LanguageRegistry（可能已被用户的 langs.xml 覆盖）。
const LanguageRegistry& FallbackRegistry() {
    static const LanguageRegistry registry;   // 构造即内置默认表
    return registry;
}


// ---------------------------------------------------------------- 语法着色的默认配色
//
// 光接上词法器不等于"用户看得见颜色"：词法器只负责把每个 token 标上样式号，
// 颜色得我们按样式号设进去。**哪些样式号需要上色**这件事统一交给
// edit/StyleCatalogue（内置语言与 UDL 一份表，同时供样式配置器列条目），
// 这里只负责把颜色发下去。
//
// 这里的默认值是"开箱可用"，用户可以改：角色级改 stylers.xml 的 <Style>，
// 单样式号改 <StyleNumber>（见 StyleScheme）。
constexpr int kCommentColour = 0x2E7D32;    // 注释：绿
constexpr int kKeywordColour = 0x0000C0;    // 关键字：深蓝
constexpr int kNumberColour = 0x986500;     // 数字：棕
constexpr int kStringColour = 0xA31515;     // 字符串：暗红
constexpr int kHighlightColour = 0x800080;  // 强调项：紫
constexpr int kExtraColour = 0x266B9E;      // 次要项：蓝

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
        // 位置也要照用：主编辑区不一定从父窗口的 (0,0) 开始（左侧还有文件夹工作区面板）。
        // 原先这里写死 (0,0)，面板一上线就表现为"编辑控件盖住了面板"——
        // 布局算得再对也没用，因为控件没被挪到那个位置
        ::MoveWindow(hwnd_, client.left, client.top, client.right - client.left,
                     client.bottom - client.top, TRUE);
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
    out.character = notification->ch;
    return true;
}

void EditView::SetCodePage(uint32_t codePage) const {
    Send(SCI_SETCODEPAGE, codePage);
}

void EditView::ConvertLineEndings(EolMode eol) const {
    SetEolMode(eol);
    // SCI_CONVERTEOLS 按当前 EOL 模式把已有的行尾整体换掉。
    // 两步都要：只设模式的话，已有内容还是旧行尾；只转换的话，之后新敲的回车又是旧的。
    Send(SCI_CONVERTEOLS, static_cast<uintptr_t>(Send(SCI_GETEOLMODE)));
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

std::string EditView::WordAtCaret() const {
    if (hwnd_ == nullptr) {
        return {};
    }
    const int32_t caret = static_cast<int32_t>(Send(SCI_GETCURRENTPOS));
    // 单词字符集用 Scintilla 自己认的那套（SCI_WORDSTARTPOSITION 的第 3 个参数为 true
    // 表示"按 SCI_SETWORDCHARS 设的字符集算"），这样与双击选词、自动完成的说法一致
    const int32_t start = static_cast<int32_t>(Send(SCI_WORDSTARTPOSITION, caret, 1));
    const int32_t end = static_cast<int32_t>(Send(SCI_WORDENDPOSITION, caret, 1));
    if (end <= start) {
        return {};
    }
    return TextRange(start, end);
}

std::string EditView::TextRange(int32_t start, int32_t end) const {
    if (end <= start) {
        return {};
    }
    // 缓冲区留一个结尾 NUL：SCI_GETTEXTRANGE 会自己补，但多给一个字节更稳妥
    std::string buffer(static_cast<std::size_t>(end - start) + 1, '\0');
    Sci_TextRangeFull range{};
    range.chrg.cpMin = start;
    range.chrg.cpMax = end;
    range.lpstrText = buffer.data();
    Send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<intptr_t>(&range));
    buffer.resize(static_cast<std::size_t>(end - start));
    return buffer;
}

bool EditView::ApplyTextTransform(
    const std::function<std::string(std::string_view)>& transform) const {
    if (hwnd_ == nullptr || !transform) {
        return false;
    }
    int32_t start = SelectionStart();
    int32_t end = SelectionEnd();
    if (start == end) {
        // 没有选区就作用于整篇：这是对标产品的行为，用户不必先全选
        start = 0;
        end = static_cast<int32_t>(ByteLength());
    }
    if (end < start) {
        std::swap(start, end);
    }
    const std::string original = TextRange(start, end);
    const std::string replaced = transform(original);
    if (replaced == original) {
        return false;   // 没有变化：不写文档，也不在撤销栈上留一步空操作
    }
    // SETSEL + REPLACESEL：一次编辑、一个撤销单元，替换完原区域仍是选区，
    // 用户可以接着对同一段内容做下一个变换
    Send(SCI_BEGINUNDOACTION);
    Send(SCI_SETSEL, static_cast<uintptr_t>(start), static_cast<intptr_t>(end));
    Send(SCI_REPLACESEL, 0, reinterpret_cast<intptr_t>(replaced.c_str()));
    Send(SCI_ENDUNDOACTION);
    return true;
}

void EditView::DuplicateCurrentLine() const {
    Send(SCI_LINEDUPLICATE);
}

void EditView::DeleteCurrentLine() const {
    Send(SCI_LINEDELETE);
}

void EditView::MoveCurrentLineUp() const {
    Send(SCI_MOVESELECTEDLINESUP);
}

void EditView::MoveCurrentLineDown() const {
    Send(SCI_MOVESELECTEDLINESDOWN);
}

void EditView::ShowCompletion(const std::vector<std::string>& words) const {
    if (words.empty()) {
        CancelCompletion();
        return;
    }
    std::string list;
    for (const std::string& word : words) {
        if (word.empty()) {
            continue;
        }
        if (!list.empty()) {
            list.push_back(' ');
        }
        list += word;
    }
    if (list.empty()) {
        CancelCompletion();
        return;
    }
    // 空格分隔、忽略大小写：用户在 "STR" 后面按补全，也该能看到 "string"
    Send(SCI_AUTOCSETSEPARATOR, ' ');
    Send(SCI_AUTOCSETIGNORECASE, 1);
    Send(SCI_AUTOCSETORDER, SC_ORDER_PERFORMSORT);
    // 第二个参数是"要被替换掉的前缀长度"：光标前已经输入的那截词
    const int32_t caret = CaretPosition();
    const int32_t start = WordStartPosition();
    const uintptr_t prefixLength =
        static_cast<uintptr_t>(caret > start ? caret - start : 0);
    Send(SCI_AUTOCSHOW, prefixLength,
         reinterpret_cast<intptr_t>(list.c_str()));
}

int32_t EditView::WordStartPosition() const {
    const int32_t caret = static_cast<int32_t>(Send(SCI_GETCURRENTPOS));
    return static_cast<int32_t>(
        Send(SCI_WORDSTARTPOSITION, static_cast<uintptr_t>(caret), 1));
}

bool EditView::CompletionActive() const {
    return Send(SCI_AUTOCACTIVE) != 0;
}

void EditView::CancelCompletion() const {
    Send(SCI_AUTOCCANCEL);
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

void EditView::ApplyPalette(const EditorPalette& palette) const {
    palette_ = palette;
    const auto colour = [](std::uint32_t rrggbb) {
        return static_cast<intptr_t>(StyleScheme::ToScintillaColour(rrggbb));
    };
    // 只设"控件皮肤"那几项；默认前景/背景与字体由 StyleScheme（当前主题）决定，
    // 不在这里重复——否则主题一换，两份设置会互相打架（见 ApplySchemeDefaults）
    Send(SCI_STYLESETFORE, STYLE_LINENUMBER, colour(palette.lineNumberFore));
    Send(SCI_STYLESETBACK, STYLE_LINENUMBER, colour(palette.lineNumberBack));
    Send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    Send(SCI_SETMARGINWIDTHN, 0, 48);
    ConfigureBookmarkMargin();
    ConfigureFoldMargin();

    Send(SCI_SETCARETFORE, colour(palette.caret));
    Send(SCI_SETCARETLINEVISIBLE, 1);
    Send(SCI_SETCARETLINEBACK, colour(palette.caretLineBack));
    Send(SCI_SETSELBACK, 1, colour(palette.selectionBack));
    if (palette.whitespace != 0) {
        Send(SCI_SETWHITESPACEFORE, 1, colour(palette.whitespace));
    } else {
        Send(SCI_SETWHITESPACEFORE, 0, 0);
    }
}

void EditView::ApplyLightTheme() const {
    ApplyPalette(EditorPalette::Light());
}

void EditView::ShowLineNumbers(bool visible) const {
    Send(SCI_SETMARGINWIDTHN, 0, visible ? 48 : 0);
}

void EditView::ConfigureBookmarkMargin() const {
    // 书签放在独立边距里：可以"在行号右侧、可点击"，而不必去动行号边距
    Send(SCI_MARKERDEFINE, static_cast<uintptr_t>(kMarkerBookmark), SC_MARK_CIRCLE);
    Send(SCI_MARKERSETFORE, static_cast<uintptr_t>(kMarkerBookmark),
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(palette_.bookmarkFore)));
    Send(SCI_MARKERSETBACK, static_cast<uintptr_t>(kMarkerBookmark),
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(palette_.bookmarkBack)));
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
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDER,
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(palette_.foldFore)));
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDER,
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(palette_.foldBack)));
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDEROPEN,
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(palette_.foldFore)));
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDEROPEN,
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(palette_.foldBack)));

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

int32_t EditView::StyleBackColor(int32_t style) const {
    if (style < 0) {
        return -1;
    }
    return static_cast<int32_t>(Send(SCI_STYLEGETBACK, static_cast<uintptr_t>(style)));
}

bool EditView::StyleBold(int32_t style) const {
    return style >= 0 && Send(SCI_STYLEGETBOLD, static_cast<uintptr_t>(style)) != 0;
}

bool EditView::StyleItalic(int32_t style) const {
    return style >= 0 && Send(SCI_STYLEGETITALIC, static_cast<uintptr_t>(style)) != 0;
}

int32_t EditView::DefaultBackColor() const {
    return static_cast<int32_t>(Send(SCI_STYLEGETBACK, static_cast<uintptr_t>(STYLE_DEFAULT)));
}

void EditView::ReadStyledText(int32_t start, int32_t length, std::string& text,
                              std::vector<int>& styles) const {
    text.clear();
    styles.clear();
    if (length <= 0 || hwnd_ == nullptr) {
        return;
    }
    // SCI_GETSTYLEDTEXT 一次把"字节 + 样式号"成对取回来（每对两个字节）。
    // 用块的方式取，是为了不为了 60 MB 的文档先要一大块内存；样式号在 Scintilla 里
    // 就是 0~255（STYLE_MAX），所以每对里的第二字节够用，不必逐字节去问（那要几十万次消息）
    constexpr int32_t kChunk = 64 * 1024;
    text.reserve(static_cast<std::size_t>(length));
    styles.reserve(static_cast<std::size_t>(length));
    std::vector<char> buffer(static_cast<std::size_t>(kChunk) * 2 + 2, '\0');
    for (int32_t offset = 0; offset < length; offset += kChunk) {
        const int32_t size = std::min(kChunk, length - offset);
        Sci_TextRange range{};
        range.chrg.cpMin = start + offset;
        range.chrg.cpMax = start + offset + size;
        range.lpstrText = buffer.data();
        const intptr_t got = Send(SCI_GETSTYLEDTEXT, 0,
                                  reinterpret_cast<intptr_t>(&range));
        if (got <= 0) {
            break;
        }
        // got 是**写入的字节数**（每个位置两个字节：一个字节内容 + 一个样式号），
        // 所以对数 = got / 2。这里曾经按 got 直接循环，于是每读一段就在末尾多出一倍长度的
        // '\0'——导出 HTML/RTF 的文件末尾一直拖着这些空字符，只是从来没人数过尾巴
        //（Scintilla 的 GetStyledText 返回的是 iPlace，也就是 2×(cpMax-cpMin)，见附录 B）
        const intptr_t pairs = got / 2;
        for (intptr_t index = 0; index < pairs; ++index) {
            const char ch = buffer[static_cast<std::size_t>(index) * 2];
            const auto style = static_cast<unsigned char>(
                buffer[static_cast<std::size_t>(index) * 2 + 1]);
            text.push_back(ch);
            styles.push_back(static_cast<int>(style));
        }
    }
}

void EditView::SetPrintColourMode(PrintColourMode mode) const {
    Send(SCI_SETPRINTCOLOURMODE, static_cast<uintptr_t>(mode));
}

void EditView::SetPrintMagnification(int magnification) const {
    Send(SCI_SETPRINTMAGNIFICATION, static_cast<uintptr_t>(magnification));
}

void EditView::SetPrintWrapMode(PrintWrapMode mode) const {
    Send(SCI_SETPRINTWRAPMODE, static_cast<uintptr_t>(mode));
}

intptr_t EditView::FormatRange(bool draw, PrintRange& range) const {
    if (hwnd_ == nullptr) {
        return -1;
    }
    Sci_RangeToFormatFull scintiRange{};
    scintiRange.hdc = static_cast<HDC>(range.hdc);
    scintiRange.hdcTarget = static_cast<HDC>(range.hdcTarget);
    scintiRange.rc.left = range.rcLeft;
    scintiRange.rc.top = range.rcTop;
    scintiRange.rc.right = range.rcRight;
    scintiRange.rc.bottom = range.rcBottom;
    scintiRange.rcPage.left = range.pageLeft;
    scintiRange.rcPage.top = range.pageTop;
    scintiRange.rcPage.right = range.pageRight;
    scintiRange.rcPage.bottom = range.pageBottom;
    scintiRange.chrg.cpMin = range.start;
    scintiRange.chrg.cpMax = range.end;
    return Send(SCI_FORMATRANGE, draw ? 1u : 0u,
                reinterpret_cast<intptr_t>(&scintiRange));
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

void EditView::ApplySchemeDefaults(const StyleScheme& scheme) const {
    // 方案里的颜色是 0xRRGGBB，控件要 COLORREF——转换就在这一步做（见 StyleScheme 的说明）
    Send(SCI_STYLESETFORE, STYLE_DEFAULT,
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(scheme.Fore())));
    Send(SCI_STYLESETBACK, STYLE_DEFAULT,
         static_cast<intptr_t>(StyleScheme::ToScintillaColour(scheme.Back())));
    // 字体名是 ASCII（方案文件里也是手写的），窄字符转换在这里是安全的
    std::string face;
    for (const wchar_t ch : scheme.FontFace()) {
        face.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    if (face.empty()) {
        face = "Consolas";
    }
    // SetDefaultFont 会 STYLECLEARALL：把默认扩到所有样式。所以必须在设角色色**之前**调用，
    // 否则会把刚设好的角色色又抹平。
    SetDefaultFont(face.c_str(), scheme.FontSize());
    // STYLECLEARALL 把行号边距、记号那几项也清回默认了，所以皮肤配色要在这里重铺一遍——
    // 不然换一次语言，深色下的行号栏就会变回白的（浅色下几乎看不出来，深色下一眼就看见）
    ApplyPalette(palette_);
}

void EditView::ApplySyntaxColours(const LanguageDef& language, const StyleScheme& scheme) const {
    const std::vector<StyleEntry> entries = StylesFor(language);
    const auto apply = [this](int style, const RoleStyle& value) {
        Send(SCI_STYLESETFORE, static_cast<uintptr_t>(style),
             static_cast<intptr_t>(StyleScheme::ToScintillaColour(value.fore)));
        Send(SCI_STYLESETBOLD, static_cast<uintptr_t>(style), value.bold ? 1 : 0);
        Send(SCI_STYLESETITALIC, static_cast<uintptr_t>(style), value.italic ? 1 : 0);
    };
    // 先按角色铺一遍……
    for (const StyleEntry& entry : entries) {
        apply(entry.number, scheme.Role(language.languageName, entry.role));
    }
    // ……再让"逐样式号"的覆盖压上去。这一步不看目录：文件里写了什么就应用什么，
    // 手写一个目录里没有的冷门样式号也能生效（见 StyleCatalogue 的说明）。
    for (const StyleOverride& item : scheme.Styles(language.languageName)) {
        apply(item.number, item.value);
    }
}

void EditView::ApplySyntaxColours(const char* lexerName) const {
    // 兼容路径：只有 lexer 名、没有语言定义，用内置默认配色
    const StyleScheme scheme = StyleScheme::Defaults();
    LanguageDef placeholder;
    placeholder.lexer = lexerName != nullptr ? lexerName : "";
    ApplySyntaxColours(placeholder, scheme);
}

void EditView::ApplyLanguage(const LanguageDef& language, const StyleScheme* scheme) const {
    if (!Valid()) {
        return;   // 控件还没建好（或已经销毁）：词法器造出来就没人接管了
    }
    languageName_ = language.displayName.c_str();
    languageKey_ = language.languageName;
    const StyleScheme& active = scheme != nullptr ? *scheme : StyleScheme::Defaults();
    ApplySchemeDefaults(active);
    // UDL 用自研词法器；内置语言走 Lexilla 的按名查找。
    Scintilla::ILexer5* lexer = language.udl != nullptr
                                    ? CreateUdlLexer(*language.udl)
                                    : CreateLexer(language.lexer.c_str());
    if (lexer == nullptr) {
        // 拿不到词法器时 Scintilla 会**静默**把词法器清掉（等于没有着色也没有折叠）。
        // 这种"写错了但不报错"的事踩过（langs.xml 里 js 指向不存在的词法器），
        // 所以在交接的地方留一条日志。
        infra::LogWarn(L"langs", L"找不到词法器 \"" +
                                     std::wstring(language.lexer.begin(), language.lexer.end()) +
                                     L"\"，语言 " + language.languageName + L" 将没有着色");
    }
    Send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(lexer));
    // 每一个组都要显式设一次：词法器的关键词表是"粘在控件上"的，新语言没定义的组
    // 如果跳过不设，就会**留着上一个语言的词表**——切换文件之后会出现"这批处理里
    // 的 set 被当成了 SQL 关键字"这种莫名其妙的着色。
    for (std::size_t group = 0; group < 9; ++group) {
        const char* words = group < language.keywords.size() && !language.keywords[group].empty()
                                ? language.keywords[group].c_str()
                                : "";
        Send(SCI_SETKEYWORDS, group, reinterpret_cast<intptr_t>(words));
    }
    if (language.foldable) {
        // 折叠层级是 lexer 自己算的，但要不要算得先告诉它。
        // fold.compact=0 是关键：默认值会把 "}" 后面没有正文的块折起来，
        // 折出来的看起来像丢了一行。
        Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold"),
             reinterpret_cast<intptr_t>("1"));
        Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold.compact"),
             reinterpret_cast<intptr_t>("0"));
        Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold.comment"),
             reinterpret_cast<intptr_t>("1"));
        const std::string_view lexerName(language.lexer);
        if (lexerName == "cpp" || lexerName == "xml" || lexerName == "hypertext") {
            // 预处理块（#if / #region）也能折
            Send(SCI_SETPROPERTY, reinterpret_cast<uintptr_t>("fold.preprocessor"),
                 reinterpret_cast<intptr_t>("1"));
        }
    }
    SetFoldingEnabled(language.foldable);
    ApplySyntaxColours(language, active);
    // 语言切换后需要整体重新着色
    Send(SCI_COLOURISE, 0, -1);
}

void EditView::ApplyLanguageForPath(const std::wstring& filePath,
                                    const LanguageRegistry* registry,
                                    const StyleScheme* scheme) const {
    const std::string extension = LanguageRegistry::ExtensionOfFile(filePath);
    const LanguageRegistry* source = registry != nullptr ? registry : &FallbackRegistry();
    if (const LanguageDef* language = source->FindByExtension(extension); language != nullptr) {
        ApplyLanguage(*language, scheme);
        return;
    }
    // 没有匹配的语言：退回纯文本
    languageName_ = L"纯文本";
    languageKey_ = L"null";
    ApplySchemeDefaults(scheme != nullptr ? *scheme : StyleScheme::Defaults());
    Scintilla::ILexer5* plain = CreateLexer("null");
    Send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(plain));
    SetFoldingEnabled(false);
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
