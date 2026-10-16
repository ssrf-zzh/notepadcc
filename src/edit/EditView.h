#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "edit/Encoding.h"
#include "edit/LanguageRegistry.h"
#include "edit/EditorPalette.h"
#include "edit/StyleScheme.h"
#include "edit/TextTools.h"

namespace npcc::edit {

// 向系统注册 Scintilla 窗口类。必须在创建任何编辑视图之前调用一次。
bool RegisterEditorClasses(void* instance);

// 外壳层需要关心的编辑器事件（数值与 Scintilla 的 SCN_* 一致，但外壳不必包含 Scintilla 头文件）
enum class EditorEvent : uint32_t {
    StyleNeeded      = 2000,
    CharAdded        = 2001,
    SavePointReached = 2002,
    SavePointLeft    = 2003,
    UpdateUI         = 2007,
    Modified         = 2008,
    MarginClick      = 2010,
    Zoom             = 2018,
    FocusIn          = 2028,
    FocusOut         = 2029,
    Unknown          = 0,
};

// 从 Scintilla 的 SCNotification 提取出来的、外壳层需要的字段。
struct EditorNotification {
    EditorEvent event = EditorEvent::Unknown;
    int32_t  position = 0;
    int32_t  modificationType = 0;
    int32_t  updated = 0;          // SCN_UPDATEUI 的 SC_UPDATE_* 位
    int32_t  line = 0;             // SCN_MARGINCLICK
    int32_t  margin = 0;           // SCN_MARGINCLICK
    int32_t  character = 0;        // SCN_CHARADDED：刚敲进去的那个字符（宏录制要用）
};

// Scintilla 的唯一适配层。全程序只有这里调用 Scintilla 消息（设计文档 2.2 第 1 条）。
class EditView {
public:
    EditView() = default;
    ~EditView();
    EditView(const EditView&) = delete;
    EditView& operator=(const EditView&) = delete;

    bool Create(HWND parent, int controlId);
    void Destroy();
    // 主窗口销毁后子窗口句柄随之失效，此时只清空记录，不再调用 DestroyWindow
    void Detach() noexcept { hwnd_ = nullptr; }
    void SetVisible(bool visible) const;
    void Resize(const RECT& client) const;

    HWND Handle() const noexcept { return hwnd_; }
    bool Valid() const noexcept { return hwnd_ != nullptr; }

    // 逃生通道：仅供插件与迁移期使用。内部代码请一律使用下面的语义化方法。
    intptr_t Send(uint32_t message, uintptr_t wParam = 0, intptr_t lParam = 0) const;

    // 把 Windows 的 WM_NOTIFY 参数翻译成 EditorNotification；不是 Scintilla 通知时返回 false。
    static bool TranslateNotify(const void* nmhdr, EditorNotification& out);

    // ---- 编码与行尾 ----
    void SetCodePage(uint32_t codePage) const;
    void SetEolMode(EolMode eol) const;

    // ---- 文本 ----
    void SetText(std::string_view bytes) const;
    std::string GetAllText() const;
    // 用 Scintilla 的 ILoader 分块装载。与 SetText 的区别在于：
    // 不会在应用层先拼一个完整副本，峰值内存少一份文件大小。
    bool LoadFromChunkedData(const char* data, std::uint64_t size, bool largeDocument) const;
    // 大文件模式下的降级：关掉最吃性能的显示项
    void ApplyLargeFileDisplayMode() const;
    // 超大文件：再叠上只读、关闭撤销、改为纯文本着色
    void ApplyHugeFileMode() const;

    // ---- 文档状态 ----
    void SetReadOnly(bool readOnly) const;
    bool IsModified() const;
    void MarkSavePoint() const;
    void ClearUndoBuffer() const;

    // ---- 外观 ----
    // 控件皮肤（边距、光标、选区、记号）用哪套配色。语法颜色由 StyleScheme 的主题决定，
    // 两者独立：用户可以整份替换 stylers.xml，而"深色下边距该是深灰"由这里兜住。
    void ApplyPalette(const EditorPalette& palette) const;
    const EditorPalette& Palette() const noexcept { return palette_; }
    void ApplyLightTheme() const;
    void SetDefaultFont(const char* faceName, int pointSize) const;
    void ShowLineNumbers(bool visible) const;
    void SetShowWhitespace(bool enabled) const;
    void SetShowEol(bool enabled) const;
    void SetIndentGuides(bool enabled) const;
    void SetCaretLineVisible(bool enabled) const;
    void SetWrapMode(bool enabled) const;
    void SetZoom(int32_t deltaSteps) const;
    int32_t ZoomLevel() const;
    void SetTabWidth(int32_t width, bool useTabs) const;

    // ---- 词法着色：按扩展名套用内置语言表（M1 会替换为 lang/ 模块的语言定义加载器） ----
    // 按扩展名套用语言。registry 为空时用内置语言表（单元测试与老调用点走这条路）。
    void ApplyLanguageForPath(const std::wstring& filePath,
                              const LanguageRegistry* registry = nullptr,
                              const StyleScheme* scheme = nullptr) const;
    // 直接按语言定义套用（应用侧走这条：定义来自用户可编辑的 langs.xml）
    // scheme 为空时用内置默认配色
    void ApplyLanguage(const LanguageDef& language, const StyleScheme* scheme = nullptr) const;
    const std::wstring& LanguageName() const noexcept { return languageKey_; }
    const wchar_t* CurrentLanguageName() const noexcept { return languageName_; }

    // ---- 查询 ----
    int32_t SelectionStart() const;
    int32_t SelectionEnd() const;
    int32_t CurrentLine() const;        // 0 基
    int32_t CurrentColumn() const;      // 0 基
    int32_t LineCount() const;
    int32_t SelectionByteCount() const;
    void    SetCaretPosition(int32_t position) const;
    void    SetFirstVisibleLine(int32_t line) const;
    int32_t FirstVisibleLine() const;
    int32_t CaretPosition() const;
    // 光标处的词（按 Scintilla 的单词字符定义）。没有词（点在空白上）返回空串。
    // 运行命令的 $(CURRENT_WORD) 用它（设计文档 2.13 第 3 条）。
    std::string WordAtCaret() const;
    void    ScrollToLine(int32_t line) const;
    int64_t PositionFromLine(int32_t line) const;

    // ---- 查找与替换支持（供 search 模块使用） ----
    // indicator 槽位分配见设计文档 4.1：0 当前匹配、1 全部匹配
    static constexpr int kIndicatorAllMatches = 1;

    // 与 Scintilla 的 SCFIND_* 对应；EditView.cpp 里有 static_assert 钉住取值，
    // 这样 search 模块不必包含 Scintilla 头文件。
    static constexpr int32_t kSearchFlagWholeWord = 0x00000002;
    static constexpr int32_t kSearchFlagMatchCase = 0x00000004;

    // 书签用的边距与标记编号。
    // 标记编号必须落在 0~24：25~31 被 Scintilla 的折叠标记占用了。
    static constexpr int kMarginBookmark = 1;
    static constexpr int kMarkerBookmark = 0;
    static constexpr int kMarginFold = 2;   // 折叠边距（Scintilla 约定：2 号边距放折叠符号）

    void        SetSearchFlags(int32_t flags) const;
    void        SetTarget(int64_t start, int64_t end) const;
    int64_t     SearchInTarget(std::string_view bytes) const;
    void        ReplaceSelection(std::string_view bytes) const;
    void        SetSelectionRange(int64_t start, int64_t end) const;
    int64_t     TextLength() const;
    std::string GetRange(int64_t start, int64_t end) const;
    void        BeginUndoAction() const;
    void        EndUndoAction() const;
    void        EnsureLineVisible(int32_t line) const;
    void        ScrollCaretToVisible() const;
    void        ConfigureSearchIndicators() const;
    void        MarkRange(int indicatorId, int64_t start, int64_t end) const;
    void        ClearIndicator(int indicatorId) const;

    // ---- 书签 ----
    void                 ConfigureBookmarkMargin() const;
    bool                 HasBookmark(int32_t line) const;
    void                 AddBookmark(int32_t line) const;
    void                 RemoveBookmark(int32_t line) const;
    void                 ToggleBookmark(int32_t line) const;
    void                 ClearAllBookmarks() const;
    int32_t              NextBookmarkLine(int32_t fromLine, bool wrap) const;   // -1 = 没有
    int32_t              PrevBookmarkLine(int32_t fromLine, bool wrap) const;
    std::vector<int32_t> AllBookmarkLines() const;

    // ---- 列编辑（设计文档 2.6） ----
    // 选项值对应 Scintilla 的 SC_SEL_* / SCVS_*，在 .cpp 里有 static_assert 钉住
    static constexpr int32_t kSelectionStream = 0;
    static constexpr int32_t kSelectionRectangle = 1;
    static constexpr int32_t kVirtualSpaceRectangle = 1;
    static constexpr int32_t kRectModifierAlt = 4;

    // 开启 Alt+鼠标拖动矩形选择、Alt+Shift+方向键扩展，以及虚拟空间
    void ConfigureColumnEditing() const;

    bool    IsRectangularSelection() const;
    void    SetSelectionMode(int32_t mode) const;
    int32_t SelectionMode() const;
    // 建立矩形选区。注意不能用 SCI_SETSEL——那条路径不保留矩形模式，
    // 实测调用后 SCI_SELECTIONISRECTANGLE 会退回 0。
    void    SetRectangularSelection(int32_t anchorPos, int32_t caretPos) const;

    // 矩形选区覆盖的行范围（0 基，含两端）
    void ColumnRange(int32_t& firstLine, int32_t& lastLine) const;
    int32_t LineSelectionStart(int32_t line) const;   // 无选区返回 -1
    int32_t LineSelectionEnd(int32_t line) const;

    void InsertTextAt(int32_t position, std::string_view text) const;
    void DeleteRangeAt(int32_t start, int32_t end) const;

    bool OverwriteMode() const;
    void SetOverwriteMode(bool enabled) const;
    void SetUndoCollection(bool enabled) const;

    // ---- 崩溃抢救专用（设计文档 8.3）----
    // 这两个接口不分配内存，可以在崩溃处理器里安全调用。
    // 常规代码请用 GetAllText：它返回 std::string，在堆可能已经坏掉的时候不能用。
    int32_t ByteLength() const;
    int32_t CopyTextTo(char* buffer, int32_t capacity) const;

    // ---- 语法着色的自检接口 ----
    // 光"接上了词法器"不等于"用户看得见颜色"：颜色要靠给每个样式号设前景色。
    // 这两个接口让测试能验证"某个位置的 token 确实拿到了与默认色不同的颜色"。
    //
    // 注意返回的是 **COLORREF（0x00BBGGRR）**，也就是控件真正拿去绘制的那个值——
    // 不是配置文件里的 #RRGGBB。故意不做回转换：只有这样，断言才是在核对"屏幕上画什么"，
    // 而"该转换的地方忘了转换"（红蓝互换）这种错误才可能被测试抓住。
    int32_t StyleAt(int32_t position) const;
    int32_t StyleForeColor(int32_t style) const;
    int32_t StyleBackColor(int32_t style) const;
    bool    StyleBold(int32_t style) const;
    bool    StyleItalic(int32_t style) const;
    int32_t DefaultForeColor() const;
    int32_t DefaultBackColor() const;

    // ---- 导出与打印（设计文档 2.14）----
    // 逐字节读回"文本 + 样式号"。导出要的是"每个字节属于哪个样式"，
    // 而这个信息只有控件知道，所以由这里提供原始数据、由 core 的纯函数去生成 HTML/RTF。
    // styles 与 text 一一对应（长度相同）。
    void ReadStyledText(int32_t start, int32_t length, std::string& text,
                        std::vector<int>& styles) const;

    // 打印着色模式与换行模式的取值（与 Scintilla 的 SC_PRINT_* 一致，编译期钉住）
    enum PrintColourMode : int {
        kPrintNormal = 0,                       // 白底黑字（省墨）
        kPrintInvertLight = 1,
        kPrintBlackOnWhite = 2,
        kPrintColourOnWhite = 3,                // 彩色、白底
        kPrintColourOnWhiteDefaultBg = 4,
        kPrintScreenColours = 5,                // 与屏幕一致（含深色主题的深底）
    };
    enum PrintWrapMode : int { kPrintWrapNone = 0, kPrintWrapWord = 1 };
    void SetPrintColourMode(PrintColourMode mode) const;
    void SetPrintMagnification(int magnification) const;
    void SetPrintWrapMode(PrintWrapMode mode) const;
    // 打印排版的输入输出。**故意只放普通字段**：MainWindow 不该知道 Scintilla 的结构体
    // （只有这一个文件允许碰 Scintilla 类型），换算由这里做。
    // 所有长度单位都是 twips（1/1440 英寸）——打印机上排版就是这么算的。
    struct PrintRange {
        void*   hdc = nullptr;         // 目标 DC（打印机）
        void*   hdcTarget = nullptr;   // 用于度量字体/颜色的 DC，通常同一个
        int32_t rcLeft = 0;
        int32_t rcTop = 0;
        int32_t rcRight = 0;
        int32_t rcBottom = 0;
        int32_t pageLeft = 0;
        int32_t pageTop = 0;
        int32_t pageRight = 0;
        int32_t pageBottom = 0;
        int32_t start = 0;             // 从哪个字节开始排
        int32_t end = 0;               // 排到哪个字节为止（通常是文档长度）
    };
    // 把 range.start 起的文本排版到 DC 上；draw=false 只测量不输出。
    // 返回**排到哪一位**（下一次从这里接着排），失败返回 -1。
    intptr_t FormatRange(bool draw, PrintRange& range) const;

    // ---- 代码折叠（设计文档 2.6）----
    // 折叠层级由 Lexilla 算出来，这里只负责呈现与开关。纯文本（含超大文件模式）
    // 没有折叠可言，所以开关是按语言给的。
    void    ConfigureFoldMargin() const;
    void    SetFoldingEnabled(bool enabled) const;
    bool    FoldingEnabled() const;
    int32_t FoldLevelOf(int32_t line) const;          // 含 SC_FOLDLEVELHEADERFLAG
    bool    LineIsFoldHeader(int32_t line) const;
    bool    IsLineFolded(int32_t line) const;
    void    ToggleFoldAt(int32_t line) const;
    void    ToggleFoldAtCaret() const;                // 在块内按快捷键时折叠包住它的那一层
    void    FoldAll() const;
    void    UnfoldAll() const;
    int32_t VisibleLineCount() const;                 // 折叠之后屏幕上实际有多少行
    // 折叠状态随会话保存：收集折叠头所在行；恢复时逐行折叠回去
    std::vector<int32_t> FoldedLines() const;
    void RestoreFoldedLines(const std::vector<int32_t>& lines) const;

    // ---- 克隆视图（设计文档 6.3 / ADR-07）----
    // 让本视图显示 source 正在编辑的那份文档：两个 Scintilla 控件共用同一个
    // Document，文本、标记、折叠状态是同一份，各自只保留自己的光标与滚动位置。
    // Scintilla 内部对 Document 做引用计数，用不着我们手工 AddRef/Release。
    bool ShareDocumentWith(const EditView& source);

    // 按 lexer 名套上默认配色。样式号取自 Lexilla 的 SCE_* 定义，
    // 是"让颜色真的看得见"的那一步（词法器本身只标样式号，不设颜色）。
    void ApplySyntaxColours(const char* lexerName) const;
    void ApplySyntaxColours(const LanguageDef& language, const StyleScheme& scheme) const;
    // 套用方案里的前景/背景/字体/字号（会先把默认样式扩散到全部样式）
    void ApplySchemeDefaults(const StyleScheme& scheme) const;

    // 行尾转换：既改"以后新输入用什么行尾"，也把现有行尾一次性换掉
    void ConvertLineEndings(EolMode eol) const;

    // ---- 行操作与文本转换（设计文档 2.13）----
    // 把一次文本变换应用到"选区（没有选区则整篇）"，并且**包在单个撤销单元里**：
    // 用户按一次 Ctrl+Z 就该整个还原，而不是一步一步回退。
    // 返回是否真的改动了内容——没变就不该动撤销栈，也不该把文档标成已修改。
    bool ApplyTextTransform(const std::function<std::string(std::string_view)>& transform) const;

    // 取一段字节（变换的输入）。指针消息只在进程内发，不存在封送问题。
    std::string TextRange(int32_t start, int32_t end) const;

    // 行操作里用 Scintilla 原生能力更快也更稳的四件事
    void DuplicateCurrentLine() const;
    void DeleteCurrentLine() const;
    void MoveCurrentLineUp() const;
    void MoveCurrentLineDown() const;

    // ---- 自动完成（设计文档 2.6 第 6 条）----
    // 候选词由外壳收集（文档内提词 + 语言词库），这里只负责把它交给 Scintilla 显示。
    // 列表用空格分隔（Scintilla 的约定），所以词里不能带空格——收集时已经保证了。
    void ShowCompletion(const std::vector<std::string>& words) const;
    bool CompletionActive() const;
    void CancelCompletion() const;
    // 光标处这个词的起点（用于取"已输入的前缀"）
    int32_t WordStartPosition() const;

    // ---- 括号匹配（设计文档 2.1 第 5 条）----
    // 判断与配对交给纯函数 FindBraceMatch，这里只负责"取哪一段文本、把位置换算回
    // 文档坐标、把结果交给 Scintilla 显示"。
    //
    // **只取光标前后各 window 个字节**：整篇读出来匹配在大文件上会把界面拖死，而现实里的
    // 配对括号不会隔几十万字节；窗口之外一律按"没配对上"处理（与纯函数的口径一致）。
    BraceMatch FindBraceNearCaret(int32_t window = 64 * 1024) const;
    // 把 FindBraceNearCaret 的结果画出来：配上了就把两个括号一起交给 SCI_BRACEHIGHLIGHT，
    // 没配上就用 SCI_BRACEBADLIGHT 把那个孤立的标出来；一个括号都没挨着就清掉高亮。
    void HighlightBracesNearCaret() const;
    void ClearBraceHighlight() const;
    // 跳到配对的括号上（Ctrl+B）。成功返回 true；没配上返回 false，由调用方给用户一个说法
    // ——设计里写的是"状态栏提示而非静默无响应"。
    bool GotoMatchingBrace() const;

    // ---- 文档统计（"摘要"用）----
    // 行数与字节数直接问 Scintilla（全量、不读数）；字符数与词数走纯函数 StatsOf，
    // 最多读 kStatsTextLimit 个字节，截断时把 truncated 置真。
    DocumentStats Statistics() const;
    static constexpr std::size_t kStatsTextLimit = 8u * 1024u * 1024u;
    uint32_t CodePage() const;   // 65001 表示缓冲区按 UTF-8 解释

private:
    HWND hwnd_ = nullptr;
    mutable const wchar_t* languageName_ = L"普通文本";
    mutable std::wstring   languageKey_ = L"null";   // 语言定义里的稳定标识，供注释等按语言的功能使用
    mutable bool           foldingEnabled_ = false;
    mutable EditorPalette  palette_ = EditorPalette::Light();
    mutable bool           largeFileMode_ = false;   // 大文件档：折叠一律关掉
};

}  // namespace npcc::edit
