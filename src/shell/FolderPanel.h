#pragma once

#include <windows.h>

#include <commctrl.h>   // HTREEITEM 等树控件类型

#include <functional>
#include <string>
#include <vector>

#include "core/FolderTree.h"

namespace npcc::shell {

// 文件夹工作区面板（设计文档 2.11）。向左停靠的一条文件树。
//
// 实现上用系统的 TreeView 控件，而不是像标签栏那样自绘：树要的东西（展开折叠、
// 键盘导航、滚动、选中态）系统控件都给齐了，自绘只会把这几样重新写一遍还写不好。
// 代价是它跟随系统配色，暗色模式下这一块仍是浅色——与"这一版管到编辑区 + 标签栏"
// 的范围一致（见设计说明书 2.6 第 4 条）。
//
// 面板本身不认识文档：双击/回车只发一条通知，由外壳决定"打开哪个文件"。
constexpr UINT kFolderNotifyCode = 0x1001u;

enum class FolderAction : int {
    OpenFile = 1,   // 双击或回车打开了某个文件
};

struct FolderNotify {
    NMHDR        hdr{};
    FolderAction action = FolderAction::OpenFile;
    std::wstring path;
};

class FolderPanel {
public:
    static constexpr const wchar_t* kClassName = L"notepadccFolderPanel";

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, int controlId);
    void Destroy();

    HWND Handle() const noexcept { return hwnd_; }
    HWND Tree() const noexcept { return tree_; }
    int  Width() const;
    bool Visible() const noexcept { return visible_; }

    // 换一个工作区目录（空 = 关闭面板）；会重建整棵树
    void SetRoot(const std::wstring& root);
    const std::wstring& Root() const noexcept { return root_; }
    // 重新扫描当前目录（外部新增/删除文件之后）
    void Refresh();
    // 选中项对应的路径（目录也可以），没有选中则返回空
    std::wstring SelectedPath() const;
    // "打开选中的那一项"：是文件就通知外壳去打开，是目录就展开/收起。
    // 双击、回车、菜单命令都走这里，免得三处各写一遍同样的判断
    void ActivateSelection();

    void Resize(const RECT& rect) const;
    void SetVisible(bool visible);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);
    void Rebuild();
    void AddChildren(HTREEITEM parent, const std::vector<npcc::core::FolderNode>& nodes);
    void NotifyOpenSelected(const std::wstring& path) const;
    // 树节点上挂着路径字符串（lParam），下面三个是遍历/取用/释放它的统一入口
    std::wstring PathOfItem(HTREEITEM item) const;
    void WalkItems(HTREEITEM item, const std::function<void(HTREEITEM)>& visit) const;
    void FreeItems(HTREEITEM item);
    int  Scale(int value) const;

    HWND         hwnd_ = nullptr;
    HWND         tree_ = nullptr;
    HFONT        font_ = nullptr;
    std::wstring root_;
    bool         visible_ = false;
};

}  // namespace npcc::shell
