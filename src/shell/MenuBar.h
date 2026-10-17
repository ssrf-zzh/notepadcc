#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace npcc::shell {

// 自绘菜单栏（现代 chrome 的第二轮外观）：
//   - 不再把 HMENU 挂到窗口上（系统菜单栏没法跟着深浅配色）；
//   - 显示的文字从 HMENU 现取，点开仍走同一份弹出菜单，命令分发与勾选态刷新不改；
//   - Alt 进入键盘激活态，左右选择、回车/下键展开、Esc 退出；Alt+助记字母直接展开；
//   - 回归可跨进程读回：WM_GETTEXT 给出「文件 | 编辑 | …」，kMsgItemCenterX 给出某项中心。
class MenuBar {
public:
    static constexpr const wchar_t* kClassName = L"notepadccMenuBar";
    static constexpr UINT kMsgItemCount = WM_APP + 80;
    static constexpr UINT kMsgItemCenterX = WM_APP + 81;

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    void Destroy();
    HWND Handle() const noexcept { return hwnd_; }

    void SetMenu(HMENU menu);
    HMENU Menu() const noexcept { return menu_; }
    void SetTheme(bool dark, bool eyeCare);
    void SetVisible(bool visible);
    bool Visible() const noexcept { return visible_; }
    int  Height() const;

    void Activate();                 // Alt：进入键盘激活态
    bool Active() const noexcept { return active_; }
    void Refresh() const;
    std::wstring VisibleText() const;

private:
    struct Item {
        std::wstring title;      // 原文（可能带 "(&F)"）
        std::wstring label;      // 平时显示的 "文件"
        wchar_t mnemonic = L'\0';
        RECT rect{};             // 客户区里的命中矩形
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    void RebuildItems();
    int  HitTest(POINT point) const;
    void OpenAt(int index);
    void Deactivate(bool focusParent);
    void Paint(HDC dc) const;
    void DrawItem(HDC dc, int index) const;

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HMENU menu_ = nullptr;
    HFONT font_ = nullptr;
    bool dark_ = false;
    bool eyeCare_ = false;
    bool visible_ = true;
    bool active_ = false;
    bool tracking_ = false;
    int  hot_ = -1;
    int  open_ = -1;
    int  focus_ = 0;
    std::vector<Item> items_;
};

}  // namespace npcc::shell
