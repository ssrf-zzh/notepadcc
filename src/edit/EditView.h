#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "edit/Encoding.h"

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
    Zoom             = 2018,
    Unknown          = 0,
};

// 从 Scintilla 的 SCNotification 提取出来的、外壳层需要的字段。
struct EditorNotification {
    EditorEvent event = EditorEvent::Unknown;
    int32_t  position = 0;
    int32_t  modificationType = 0;
    int32_t  updated = 0;          // SCN_UPDATEUI 的 SC_UPDATE_* 位
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

    // ---- 文档状态 ----
    void SetReadOnly(bool readOnly) const;
    bool IsModified() const;
    void MarkSavePoint() const;
    void ClearUndoBuffer() const;

    // ---- 外观 ----
    void ApplyLightTheme() const;
    void SetDefaultFont(const char* faceName, int pointSize) const;
    void ShowLineNumbers(bool visible) const;
    void SetShowWhitespace(bool enabled) const;
    void SetShowEol(bool enabled) const;
    void SetWrapMode(bool enabled) const;
    void SetZoom(int32_t deltaSteps) const;
    int32_t ZoomLevel() const;
    void SetTabWidth(int32_t width, bool useTabs) const;

    // ---- 词法着色：按扩展名套用内置语言表（M1 会替换为 lang/ 模块的语言定义加载器） ----
    void ApplyLanguageForPath(const std::wstring& filePath) const;
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
    void    ScrollToLine(int32_t line) const;
    int64_t PositionFromLine(int32_t line) const;

    // ---- 查找与替换支持（供 search 模块使用） ----
    // indicator 槽位分配见设计文档 4.1：0 当前匹配、1 全部匹配
    static constexpr int kIndicatorAllMatches = 1;

    // 与 Scintilla 的 SCFIND_* 对应；EditView.cpp 里有 static_assert 钉住取值，
    // 这样 search 模块不必包含 Scintilla 头文件。
    static constexpr int32_t kSearchFlagWholeWord = 0x00000002;
    static constexpr int32_t kSearchFlagMatchCase = 0x00000004;

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

private:
    HWND hwnd_ = nullptr;
    mutable const wchar_t* languageName_ = L"普通文本";
};

}  // namespace npcc::edit
