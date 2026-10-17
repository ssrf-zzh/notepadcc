#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/Command.h"
#include "core/ToolbarItems.h"

namespace npcc::shell {

// 自绘工具栏（设计文档 2.11 第 6 条、1.4 的 Toolbar.*）。
//
// 为什么自绘：与其它自绘控件同一条理由——外观要跟着主题走（浅色 / 深色两套底），
// 而且**要能跨进程读回来**：WM_GETTEXT 给出"此刻栏里有哪些按钮"，
// 界面回归据此逐条核对，而不是只看命令发出去没有。
//
// 按钮的集合与顺序来自配置（core::ToolbarItems），存的是**命令键**：
// 宏、运行命令、插件命令注册进来之后照样能被摆到工具栏上——这正是设计里
// "工具栏与右键菜单定制"要顺带解决的三个遗留。
class ToolBar {
public:
    static constexpr const wchar_t* kClassName = L"notepadccToolBar";

    // 跨进程查询（只传值 / 只返回值，不封送指针）：
    //   kMsgItemCount   → 可点的按钮个数（分隔符不算）
    //   kMsgItemCenterX → wParam = 按钮序号，返回它在客户区里的中心 x；越界返回 -1
    // 界面回归据此把鼠标**真的点到**某个按钮上——命中测试因此也一起被验证
    static constexpr UINT kMsgItemCount = WM_APP + 60;
    static constexpr UINT kMsgItemCenterX = WM_APP + 61;

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    HWND Handle() const noexcept { return hwnd_; }

    void SetVisible(bool visible);
    bool Visible() const noexcept { return visible_; }
    int  Height() const;      // 不可见时是 0（布局据此决定要不要留这一条）
    int  Width() const;       // 所有按钮排下来的总宽度

    void SetItems(const core::ToolbarItems& keys);
    void SetIconSize(int logicalPixels);
    void SetDark(bool dark);
    void SetEyeCare(bool eyeCare);
    void Refresh() const;     // 可用 / 勾选状态变了就重画

    // 外壳接上来的四件事：这条命令现在能不能用、有没有勾、显示成什么、
    // 以及真点了按钮之后该做什么
    void SetEnabledQuery(std::function<bool(core::CmdId)> query) { enabledQuery_ = std::move(query); }
    void SetCheckedQuery(std::function<bool(core::CmdId)> query) { checkedQuery_ = std::move(query); }
    void SetTextQuery(std::function<std::wstring(core::CmdId)> query) { textQuery_ = std::move(query); }
    void SetInvoke(std::function<void(core::CmdId)> invoke) { invoke_ = std::move(invoke); }
    // 键 → 命令 ID：工具栏自己不认识注册表，这一步由外壳接进来
    void SetKeyResolver(std::function<core::CmdId(const std::wstring&)> resolver) {
        keyResolver_ = std::move(resolver);
    }

    // 回归读回用：按钮文本用 " | " 串起来，分隔符写成 "---"
    std::wstring VisibleText() const;

private:
    struct Item {
        core::CmdId  id = 0;     // 0 = 分隔符
        std::wstring key;
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    int  CellSize() const;                  // 一个按钮的边长（正方形）
    int  SeparatorWidth() const;
    int  ButtonCount() const;
    int  ItemLeft(std::size_t index) const; // 第 index 项的左边（客户区坐标）
    int  HitTest(int x) const;              // 返回 items_ 下标，没命中返回 -1
    int  ButtonOrdinal(int itemIndex) const;// items_ 下标 → 第几个"可点按钮"（分隔符返回 -1）
    int  OrdinalItem(int ordinal) const;    // 第几个可点按钮 → items_ 下标；越界 -1
    bool EnabledAt(int itemIndex) const;
    bool CheckedAt(int itemIndex) const;
    std::wstring TextAt(int itemIndex) const;
    void Invoke(int itemIndex);
    void Paint(HDC dc) const;
    void UpdateTooltip(int itemIndex);
    void HideTooltip();

    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    HFONT font_ = nullptr;
    bool dark_ = false;
    bool eyeCare_ = false;
    bool visible_ = true;
    int  iconSize_ = 20;     // 逻辑像素
    int  hot_ = -1;
    int  pressed_ = -1;
    bool tracking_ = false;  // 已经向系统要过 WM_MOUSELEAVE 了
    bool tipShown_ = false;
    std::vector<Item> items_;
    std::wstring tipText_;
    std::function<bool(core::CmdId)> enabledQuery_;
    std::function<bool(core::CmdId)> checkedQuery_;
    std::function<std::wstring(core::CmdId)> textQuery_;
    std::function<void(core::CmdId)> invoke_;
    std::function<core::CmdId(const std::wstring&)> keyResolver_;
};

}  // namespace npcc::shell
