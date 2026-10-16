#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "io/MappedFile.h"

namespace npcc::shell {

// 只读十六进制视图（设计文档 2.13 第 5 条）。
//
// 为什么是自绘而不是"把十六进制文本塞进一个 Scintilla"：8 MB 的文件变成十六进制文本
// 要 40 MB 上下，而二进制文件往往还更大；自绘只画窗口里看得见的那几行，内存占用与
// 文件大小无关（底下是内存映射，不复制内容）。
//
// **它读的是磁盘上的字节**，不是编辑缓冲区：编码转换（ANSI / UTF-16 → UTF-8）之后的
// 缓冲区已经不是文件里那些字节了。所以未保存或已修改的文档不给开十六进制视图，
// 而不是"看着像就行"。
class HexView {
public:
    static void RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    HWND Handle() const noexcept { return hwnd_; }

    // 打开文件（映射，不复制）。失败时 error 说明原因。
    bool OpenFile(const std::wstring& path, std::wstring* error);
    void CloseFile();
    bool Empty() const noexcept { return size_ == 0; }
    std::uint64_t FileSize() const noexcept { return size_; }
    const std::wstring& FilePath() const noexcept { return path_; }

    void SetVisible(bool visible) const;
    void Resize(const RECT& area) const;
    void SetDark(bool dark);
    void Focus() const;

    // 跳到某个字节（越界会夹到合法范围），并把它滚进可见区
    void GoTo(std::uint64_t offset);
    std::uint64_t CurrentOffset() const noexcept { return caret_; }
    // 问一次"跳到哪个偏移"并跳过去（菜单项与 Ctrl+G 都走这里）
    void PromptJump();

    // 光标字节变了就回调一次（外壳用它更新状态栏）
    void SetOffsetCallback(std::function<void(std::uint64_t)> callback) {
        offsetCallback_ = std::move(callback);
    }

    // 当前窗口里**渲染出来的那几行**（WM_GETTEXT 也返回它）。
    // 让控件能被读回来，是这一段能做端到端验证的原因：界面回归脚本跨进程发
    // WM_GETTEXT（系统消息会被封送），拿到的就是屏幕上那几行字。
    std::string VisibleText() const;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    int  OffsetWidth() const;
    int  RowCount() const;
    int  VisibleRowCount() const;
    std::uint64_t TopOffset() const;
    int  RowBytesAt(std::uint64_t offset) const;
    const std::uint8_t* BytesAt(std::uint64_t offset) const;
    void UpdateScrollBar() const;
    void EnsureCaretVisible();
    void ScrollTo(std::uint64_t topRow);
    void SetCaret(std::uint64_t offset, bool notify);
    void Paint(HDC dc, const RECT& client);
    void HandleKey(WPARAM key);
    void HandleClick(LPARAM position);
    void PromptForOffset();

    HWND          hwnd_ = nullptr;
    HWND          parent_ = nullptr;
    io::MappedFile file_;
    std::wstring  path_;
    std::uint64_t size_ = 0;
    std::uint64_t topRow_ = 0;      // 第一行可见的行号
    std::uint64_t caret_ = 0;       // 光标所在字节的偏移
    bool          dark_ = false;
    bool          dragging_ = false;
    HFONT         font_ = nullptr;
    int           charWidth_ = 8;
    int           charHeight_ = 16;
    int           dpi_ = 96;
    std::function<void(std::uint64_t)> offsetCallback_;
};

}  // namespace npcc::shell
