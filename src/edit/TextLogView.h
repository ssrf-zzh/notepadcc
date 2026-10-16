#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "edit/EditorPalette.h"
#include "edit/LineIndex.h"
#include "io/MappedFile.h"
#include "search/SearchConfig.h"

namespace npcc::edit {

// T3 超大文件的**自绘只读文本视口**（设计说明书 6.1 T3 实现要点 1/3/4/5）。
//
// 与十六进制查看（shell/HexView）同一家族：底下是内存映射，不复制内容，
// 屏幕上只画看得见的那几行——内存占用与文件大小脱钩，GB 级日志也能滚动。
// 区别是这里画的是**文本**：
//   · 打开时启动延迟行索引（LineIndex），按块扫描，边扫边显示进度；
//   · 视野内每行按文档编码解码成 UTF-16 再自绘，带行号与字节偏移；
//   · 查找对映射内存分块匹配（search::ByteMatcher，与编辑器查找同一套
//     字面量/正则/大小写/全词规则），命中后跳过去并高亮；
//   · 只读：敲字符、粘贴等编辑动作只回调"用户想编辑"，由外壳问是否以副本打开。
//
// **WM_GETTEXT 返回当前屏幕上渲染的那几行**（与 HexView 同一约定），界面回归
// 脚本据此跨进程核对"屏幕上真正显示的内容"，而不是"命令发出去了"。
class TextLogView {
public:
    // 一处查找命中。查找在后台线程里跑，结果经窗口消息回到 UI 线程再回调。
    struct FindHit {
        bool          found   = false;
        bool          wrapped = false;
        std::uint64_t matchStart = 0;   // 命中区间的字节偏移（内容坐标，不含 BOM）
        std::uint64_t matchEnd   = 0;
        std::uint64_t line       = 0;   // 命中所在行（0 起）
        std::wstring  error;            // 失败原因（正则写错、索引没建完之类）
    };

    static void RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    void Destroy();
    ~TextLogView();
    HWND Handle() const noexcept { return hwnd_; }

    // 打开文件（映射，不复制）。bomSkip 是内容起点（UTF-8 BOM 的 3 个字节），
    // 对外显示的字节偏移会加回它——用户看到的是**文件里的真实偏移**。
    bool OpenFile(const std::wstring& path, std::uint64_t bomSkip, std::wstring* error);
    void CloseFile();
    // 把磁盘上的原始字节（含 BOM）复制一份到目标路径：先写临时文件再原子替换。
    // "另存为副本"用它——不经过解码/编码，字节一个不差。
    bool WriteCopyTo(const std::wstring& path, std::wstring* error) const;
    bool Opened() const noexcept { return file_.Valid(); }
    std::uint64_t FileSize() const noexcept { return bomSkip_ + size_; }
    // 内容起点在文件里的位置（UTF-8 BOM 的 3 个字节）。对外显示的偏移都要加回它
    std::uint64_t BomSkip() const noexcept { return bomSkip_; }
    const std::wstring& FilePath() const noexcept { return path_; }

    void SetPalette(const EditorPalette& palette);
    void SetCodePage(std::uint32_t codePage);   // 65001 = UTF-8
    void SetTabWidth(int width);
    void SetVisible(bool visible) const;
    void Resize(const RECT& area) const;
    void Focus() const;

    // ---- 定位 ----
    void GoToLine(std::uint64_t line, bool scroll);   // 行号 1 起，越界夹紧
    std::uint64_t CurrentLine() const noexcept { return caretLine_ + 1; }   // 1 起
    std::uint64_t TopLine() const noexcept { return topLine_; }             // 0 起
    void SetTopLine(std::uint64_t line);
    void EnsureCaretVisible();
    void ClearHit();

    // ---- 查找（异步）----
    void FindNext(const search::SearchConfig& config, bool forward, bool wrap);
    void CountMatches(const search::SearchConfig& config);
    // 找完 / 数完各调一次。回调发生在窗口消息里（UI 线程），可以直接操作界面。
    void SetFindCallback(std::function<void(const FindHit&)> callback);
    void SetCountCallback(
        std::function<void(std::uint64_t count, bool truncated, const std::wstring& error)>
            callback);
    // 行/偏移/命中/索引进度变了就调一次（外壳据此刷新状态栏）
    void SetStatusCallback(std::function<void()> callback);
    // 用户试图编辑（敲字符 / 粘贴 / Backspace / 回车等）
    void SetEditAttemptCallback(std::function<void()> callback);

    bool IndexComplete() const noexcept { return index_.Complete(); }
    double IndexProgress() const noexcept { return index_.Progress(); }
    std::uint64_t LineCount() const noexcept { return index_.LineCount(); }
    // 光标所在行行首的字节偏移（内容坐标；状态栏显示时会加回 BOM）
    std::uint64_t CurrentOffset() const;
    std::uint64_t ContentOffsetOfLine(std::uint64_t line) const;
    // 光标所在行的文本（菜单里的"复制"与视图内 Ctrl+C 都用它）
    std::wstring CurrentLineText() const;

    // 屏幕上那几行（行号 + 十六进制偏移 + 文本，WM_GETTEXT 也返回它）
    std::string VisibleText() const;

private:
    struct FindTask;

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    int  VisibleRowCount() const;
    int  GutterColumns() const;         // 行号列 + 偏移列一共占多少个字符
    std::uint64_t MaxTopLine() const;
    std::uint64_t TextByteLimit() const;   // 屏幕上能显示的列数换算成的解码上限（字节）
    void UpdateScrollBar() const;
    void ScrollTo(std::uint64_t topLine);
    void SetCaretLine(std::uint64_t line, bool notify);
    void Paint(HDC dc, const RECT& client);
    void HandleKey(WPARAM key);
    void HandleClick(LPARAM position);

    // 解码 [start, end) 的字节为宽字符，并把 Tab 展开成空格。
    // byteLimit 是解码上限：屏幕外的部分不浪费（单行几个 GB 的日志靠它保命）；
    // startColumn 是这段字节在屏幕上的起始列——Tab 的展开要对齐到它，
    // 否则命中高亮的列会与正文错位。
    std::wstring DecodeRange(std::uint64_t start, std::uint64_t end,
                             std::uint64_t byteLimit, int startColumn = 0) const;
    std::wstring FormatOffset(std::uint64_t contentOffset) const;   // 十六进制，8/12 位
    // 屏幕上第 row 行的行号/偏移/文本（VisibleText 与测试都用它复核）
    bool LineColumns(std::uint64_t line, std::wstring* number, std::wstring* offset,
                     std::wstring* text) const;

    void StartIndexing();
    void StopIndexing();
    static void RunFindTask(std::shared_ptr<FindTask> task);
    void StartFindTask(const search::SearchConfig& config, bool forward, bool wrap, bool countOnly);
    void CancelFindTask();
    void ApplyFindResult(const FindTask& task);

    HWND           hwnd_ = nullptr;
    HWND           parent_ = nullptr;
    io::MappedFile file_;
    std::wstring   path_;
    std::uint64_t  bomSkip_ = 0;
    std::uint64_t  size_ = 0;           // 内容字节数（不含 BOM）

    LineIndex      index_;

    std::uint32_t  codePage_ = CP_UTF8;
    int            tabWidth_ = 4;
    EditorPalette  palette_ = EditorPalette::Light();

    std::uint64_t  topLine_ = 0;
    std::uint64_t  caretLine_ = 0;
    bool           hitActive_ = false;
    std::uint64_t  hitStart_ = 0;
    std::uint64_t  hitEnd_ = 0;

    HFONT          font_ = nullptr;
    int            charWidth_ = 8;
    int            charHeight_ = 16;
    int            dpi_ = 96;
    bool           dragging_ = false;

    std::function<void(const FindHit&)>      findCallback_;
    std::function<void(std::uint64_t, bool, const std::wstring&)> countCallback_;
    std::function<void()>                    statusCallback_;
    std::function<void()>                    editAttempt_;

    std::shared_ptr<FindTask> findTask_;
    std::thread               findThread_;
};

}  // namespace npcc::edit
