#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/Ids.h"

namespace npcc::shell {

// 标签栏通过 WM_NOTIFY 向外壳汇报，沿用 Win32 控件的惯例：
//   hdr.hwndFrom == 标签栏窗口，hdr.code == kTabBarNotifyCode
// 这样外壳的 WM_NOTIFY 分发逻辑可以统一处理编辑器与标签栏。
constexpr UINT kTabBarNotifyCode = 0x1000u;

enum class TabAction : int {
    Activate = 1,
    Close,
    CloseOthers,
    CloseAll,
    CloseRight,
    CopyPath,
    Reorder,        // source = 被拖动者，target = 放到它前面（kInvalidId 表示移到末尾）
};

struct TabBarNotify {
    NMHDR           hdr{};
    TabAction       action = TabAction::Activate;
    npcc::core::BufferId source = 0;
    npcc::core::BufferId target = 0;
};

struct TabItem {
    npcc::core::BufferId id = 0;
    std::wstring         title;
    bool                 modified = false;
    bool                 hasPath = false;
};

// 自绘标签栏（设计文档 2.4 第 4 条）：拖拽排序、中键关闭、右键菜单、
// 脏标记。宽度不足时可用滚轮横向滚动。
class TabBar {
public:
    static constexpr const wchar_t* kClassName = L"notepadccTabBar";

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    void Destroy();

    HWND Handle() const noexcept { return hwnd_; }
    int  Height() const;

    void SetItems(std::vector<TabItem> items, npcc::core::BufferId active);
    void SetActive(npcc::core::BufferId active);
    void EnsureActiveVisible();

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    void RebuildMetrics() const;
    int  TabWidth(size_t index) const;
    int  IndexFromPoint(POINT point) const;
    int  CloseHitIndex(POINT point) const;
    RECT ItemRect(size_t index) const;
    void EnsureVisible(size_t index);
    void ScrollBy(int delta);
    int  TotalWidth() const;

    void Notify(TabAction action, npcc::core::BufferId source,
                npcc::core::BufferId target = 0) const;
    void ShowContextMenu(POINT screenPoint, size_t index);
    void Paint(HDC target, const RECT& client);

    HWND                 hwnd_ = nullptr;
    HFONT                font_ = nullptr;
    std::vector<TabItem> items_;
    npcc::core::BufferId active_ = 0;
    int                  scrollOffset_ = 0;
    int                  dragIndex_ = -1;
    int                  dropIndex_ = -1;
    int                  hotIndex_ = -1;
    int                  hotCloseIndex_ = -1;
    bool                 dragging_ = false;
    POINT                dragOrigin_{};

    // 由字体实时测得的度量，随 DPI / 字体变化重建
    mutable int tabHeight_ = 0;
    mutable int paddingX_ = 0;
    mutable int minTabWidth_ = 0;
    mutable int maxTabWidth_ = 0;
    mutable int closeBoxSize_ = 0;
    mutable std::vector<int> tabWidths_;   // 与 items_ 一一对应，随字体与标题变化重算
};

}  // namespace npcc::shell
