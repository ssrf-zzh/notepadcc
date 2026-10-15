#include "edit/EditView.h"

#include <windows.h>

#include <Scintilla.h>
// Lexilla.h 依赖 ILexer.h 中的 ILexer5（它自己不包含该头文件），必须先引入
#include <ILexer.h>
#include <Lexilla.h>

#include <cwctype>
#include <string>
#include <vector>

namespace npcc::edit {
namespace {

// M0 的最小语言表：扩展名 -> (Lexilla lexer 名, 关键词组 0)。
// M1 起由 lang/LexerRegistry 从 langs.xml 载入，这里只是让 M0 有可见的语法着色。
struct BuiltinLanguage {
    const wchar_t* extension;
    const char*    lexer;
    const char*    keywords;
    const wchar_t* displayName;
};

const BuiltinLanguage kBuiltinLanguages[] = {
    {L"cpp", "cpp", "alignas alignof auto bool break case catch class const constexpr continue "
                   "default delete do double else enum explicit extern false float for friend goto "
                   "if inline int long namespace new nullptr operator private protected public "
                   "return short signed sizeof static struct switch template this throw true try "
                   "typedef typename union unsigned using virtual void volatile while",
     L"C/C++"},
    {L"c",   "cpp", nullptr, L"C/C++"},
    {L"h",   "cpp", nullptr, L"C/C++"},
    {L"hpp", "cpp", nullptr, L"C/C++"},
    {L"py",  "python",
     "and as assert async await break class continue def del elif else except False finally for "
     "from global if import in is lambda None nonlocal not or pass raise return True try while "
     "with yield",
     L"Python"},
    {L"js",  "javascript",
     "async await break case catch class const continue debugger default delete do else export "
     "extends false finally for function if import in instanceof let new null return super switch "
     "this throw true try typeof undefined var void while with yield",
     L"JavaScript"},
    {L"ts",  "cpp", nullptr, L"TypeScript"},
    {L"json","json", nullptr, L"JSON"},
    {L"xml", "xml", nullptr, L"XML"},
    {L"html","hypertext", nullptr, L"HTML"},
    {L"htm", "hypertext", nullptr, L"HTML"},
    {L"css", "css", nullptr, L"CSS"},
    {L"md",  "markdown", nullptr, L"Markdown"},
    {L"sql", "sql",
     "select from where insert into values update set delete create table database index view "
     "drop alter join left right inner outer on group by order having union all distinct as",
     L"SQL"},
    {L"bat", "batch", nullptr, L"Batch"},
    {L"cmd", "batch", nullptr, L"Batch"},
    {L"ps1", "powershell", nullptr, L"PowerShell"},
    {L"ini", "props", nullptr, L"配置文件"},
    {L"log", "null", nullptr, L"日志"},
};

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

    Send(SCI_SETCARETFORE, RGB(0x00, 0x00, 0x00));
    Send(SCI_SETCARETLINEVISIBLE, 1);
    Send(SCI_SETCARETLINEBACK, RGB(0xF2, 0xF6, 0xFA));
    Send(SCI_SETSELBACK, 1, RGB(0xCC, 0xE0, 0xF5));
    SetTabWidth(4, false);
}

void EditView::ShowLineNumbers(bool visible) const {
    Send(SCI_SETMARGINWIDTHN, 0, visible ? 48 : 0);
}

void EditView::SetShowWhitespace(bool enabled) const {
    Send(SCI_SETVIEWWS, enabled ? 1u : 0u);
    Send(SCI_SETINDENTATIONGUIDES, enabled ? 1u : 0u);
}

void EditView::SetShowEol(bool enabled) const {
    Send(SCI_SETVIEWEOL, enabled ? 1u : 0u);
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

void EditView::ApplyLanguageForPath(const std::wstring& filePath) const {
    const BuiltinLanguage* language = FindLanguage(ExtensionOf(filePath), nullptr);
    if (language == nullptr) {
        languageName_ = L"普通文本";
        Scintilla::ILexer5* plain = CreateLexer("null");
        Send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(plain));
        return;
    }
    languageName_ = language->displayName;
    Scintilla::ILexer5* lexer = CreateLexer(language->lexer);
    Send(SCI_SETILEXER, 0, reinterpret_cast<intptr_t>(lexer));
    if (language->keywords != nullptr) {
        Send(SCI_SETKEYWORDS, 0, reinterpret_cast<intptr_t>(language->keywords));
    }
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

void EditView::ScrollToLine(int32_t line) const {
    Send(SCI_GOTOLINE, static_cast<uintptr_t>(line));
}

}  // namespace npcc::edit
