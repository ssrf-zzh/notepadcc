#pragma once

#include <windows.h>

#include <commctrl.h>

#include <string>
#include <vector>

#include "core/FunctionList.h"

namespace npcc::shell {

// 函数列表面板（设计文档 2.6 第 5 条）：把"结构提取"的结果画成一棵树，选中就跳过去。
//
// 与文件夹工作区面板同样用系统 TreeView：层级、展开折叠、键盘导航都由控件给，
// 面板自己只负责"把符号表填进去"与"把选中项的行号报给外壳"。
// 它不认识 Scintilla——跳转是外壳的事。
constexpr UINT kFunctionNotifyCode = 0x1002u;

enum class FunctionAction : int {
    JumpToLine = 1,
};

struct FunctionNotify {
    NMHDR        hdr{};
    FunctionAction action = FunctionAction::JumpToLine;
    int          line = 0;
};

class FunctionListPanel {
public:
    static constexpr const wchar_t* kClassName = L"notepadccFunctionPanel";

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    void Destroy();

    HWND Handle() const noexcept { return hwnd_; }
    HWND Tree() const noexcept { return tree_; }
    // 面板要占的宽度（按自己的 DPI 缩放）；不可见时是 0
    int  Width() const;
    bool Visible() const noexcept { return visible_; }
    void SetVisible(bool visible);
    void Resize(const RECT& rect) const;

    // 换一批符号（按 depth 建层）。空表就清空
    void SetSymbols(const std::vector<npcc::core::FunctionSymbol>& symbols);
    // 选中项对应的行号；没选中返回 -1
    int SelectedLine() const;
    // "跳过去"：双击、回车、菜单命令都走这里
    void ActivateSelection();

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);
    void FreeItems(HTREEITEM item);
    int  Scale(int value) const;

    HWND  hwnd_ = nullptr;
    HWND  tree_ = nullptr;
    HFONT font_ = nullptr;
    bool  visible_ = false;
};

}  // namespace npcc::shell
