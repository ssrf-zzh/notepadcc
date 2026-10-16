#include "shell/FolderPanel.h"

#include <commctrl.h>

#include "infra/Log.h"

#include <algorithm>
#include <cwchar>
#include <functional>

namespace npcc::shell {
namespace {

constexpr int kTreeControlId = 1;

int DpiOf(HWND reference) {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr && reference != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr) {
            const UINT actual = getDpi(reference);
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return static_cast<int>(dpi);
}

}  // namespace

bool FolderPanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &FolderPanel::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int FolderPanel::Scale(int value) const {
    return ::MulDiv(value, DpiOf(hwnd_), 96);
}

bool FolderPanel::Create(HWND parent, int controlId) {
    if (hwnd_ != nullptr) {
        return true;
    }
    hwnd_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, kClassName, L"",
                              WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

    tree_ = ::CreateWindowExW(0, WC_TREEVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                                  TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS |
                                  TVS_SHOWSELALWAYS,
                              0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeControlId)),
                              ::GetModuleHandleW(nullptr), nullptr);
    if (tree_ == nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    ::SendMessageW(tree_, TVM_SETEXTENDEDSTYLE, 0, TVS_EX_DOUBLEBUFFER);

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    LOGFONTW logFont{};
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0) {
        logFont = metrics.lfMessageFont;
    } else {
        ::wcscpy_s(logFont.lfFaceName, L"Microsoft YaHei UI");
        logFont.lfHeight = -12;
    }
    font_ = ::CreateFontIndirectW(&logFont);
    if (font_ != nullptr) {
        ::SendMessageW(tree_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    return true;
}

void FolderPanel::Destroy() {
    if (hwnd_ != nullptr) {
        if (::IsWindow(hwnd_) != 0) {
            ::DestroyWindow(hwnd_);   // 子控件随父窗口销毁
        }
        hwnd_ = nullptr;
    }
    tree_ = nullptr;
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

int FolderPanel::Width() const {
    return visible_ ? Scale(220) : 0;
}

void FolderPanel::Resize(const RECT& rect) const {
    if (hwnd_ != nullptr && tree_ != nullptr) {
        ::MoveWindow(hwnd_, rect.left, rect.top, rect.right - rect.left,
                     rect.bottom - rect.top, TRUE);
        RECT client{};
        ::GetClientRect(hwnd_, &client);
        ::MoveWindow(tree_, 0, 0, client.right, client.bottom, TRUE);
    }
}

void FolderPanel::SetVisible(bool visible) {
    visible_ = visible && !root_.empty();
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible_ ? SW_SHOW : SW_HIDE);
    }
}

void FolderPanel::SetRoot(const std::wstring& root) {
    root_ = root;
    visible_ = !root_.empty();
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible_ ? SW_SHOW : SW_HIDE);
    }
    Rebuild();
}

void FolderPanel::Refresh() {
    Rebuild();
}

void FolderPanel::Rebuild() {
    if (tree_ == nullptr) {
        return;
    }
    // 重建之前记下"展开过哪些目录"，重建之后按路径恢复——刷新一次就把用户展开的层级
    // 全收起来，是这一类面板最招人烦的行为
    std::vector<std::wstring> expanded;
    const HTREEITEM oldRoot = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    if (oldRoot != nullptr) {
        WalkItems(oldRoot, [&expanded, this](HTREEITEM item) {
            if (::SendMessageW(tree_, TVM_GETITEMSTATE, reinterpret_cast<WPARAM>(item),
                               TVIS_EXPANDED) != 0) {
                expanded.push_back(PathOfItem(item));
            }
        });
    }

    ::SendMessageW(tree_, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));
    if (root_.empty()) {
        return;
    }
    const npcc::core::FolderNode tree = npcc::core::BuildFolderTree(root_);
    if (tree.Empty()) {
        return;
    }
    TVINSERTSTRUCTW insert{};
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<wchar_t*>(tree.name.c_str());
    insert.item.lParam = 0;
    const HTREEITEM rootItem =
        reinterpret_cast<HTREEITEM>(::SendMessageW(tree_, TVM_INSERTITEMW, 0,
                                                   reinterpret_cast<LPARAM>(&insert)));
    if (rootItem != nullptr) {
        AddChildren(rootItem, tree.children);
        ::SendMessageW(tree_, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(rootItem));
        if (!expanded.empty()) {
            // 根目录永远展开（它就是工作区），其余按刚才记下的路径恢复
            WalkItems(rootItem, [&expanded, this](HTREEITEM item) {
                const std::wstring path = PathOfItem(item);
                if (std::find(expanded.begin(), expanded.end(), path) != expanded.end()) {
                    ::SendMessageW(tree_, TVM_EXPAND, TVE_EXPAND,
                                   reinterpret_cast<LPARAM>(item));
                }
            });
        }
    }
}

std::wstring FolderPanel::PathOfItem(HTREEITEM item) const {
    if (tree_ == nullptr || item == nullptr) {
        return {};
    }
    TVITEMW info{};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    if (::SendMessageW(tree_, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&info)) == 0) {
        return {};
    }
    const auto* stored = reinterpret_cast<const std::wstring*>(info.lParam);
    return stored != nullptr ? *stored : std::wstring();
}

void FolderPanel::WalkItems(HTREEITEM item, const std::function<void(HTREEITEM)>& visit) const {
    if (tree_ == nullptr || item == nullptr) {
        return;
    }
    visit(item);
    HTREEITEM child = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_CHILD,
                       reinterpret_cast<LPARAM>(item)));
    while (child != nullptr) {
        WalkItems(child, visit);
        child = reinterpret_cast<HTREEITEM>(::SendMessageW(
            tree_, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
    }
}

void FolderPanel::FreeItems(HTREEITEM item) {
    if (tree_ == nullptr || item == nullptr) {
        return;
    }
    TVITEMW info{};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    if (::SendMessageW(tree_, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&info)) != 0) {
        delete reinterpret_cast<std::wstring*>(info.lParam);
        info.lParam = 0;
        ::SendMessageW(tree_, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&info));
    }
    HTREEITEM child = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_CHILD,
                       reinterpret_cast<LPARAM>(item)));
    while (child != nullptr) {
        HTREEITEM next = reinterpret_cast<HTREEITEM>(::SendMessageW(
            tree_, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
        FreeItems(child);
        child = next;
    }
}

void FolderPanel::AddChildren(HTREEITEM parent,
                              const std::vector<npcc::core::FolderNode>& nodes) {
    for (const npcc::core::FolderNode& node : nodes) {
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t*>(node.name.c_str());
        // 路径存在 lParam 上：CString 之类的外部存储没必要，指针在重建时一起换掉
        auto* stored = new std::wstring(node.path);
        insert.item.lParam = reinterpret_cast<LPARAM>(stored);
        const HTREEITEM item =
            reinterpret_cast<HTREEITEM>(::SendMessageW(tree_, TVM_INSERTITEMW, 0,
                                                       reinterpret_cast<LPARAM>(&insert)));
        if (item == nullptr) {
            delete stored;
            continue;
        }
        if (!node.children.empty()) {
            AddChildren(item, node.children);
        }
    }
}

std::wstring FolderPanel::SelectedPath() const {
    if (tree_ == nullptr) {
        return {};
    }
    const HTREEITEM item = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_CARET, 0));
    if (item == nullptr) {
        return {};
    }
    TVITEMW info{};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    if (::SendMessageW(tree_, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&info)) == 0) {
        return {};
    }
    const auto* stored = reinterpret_cast<const std::wstring*>(info.lParam);
    return stored != nullptr ? *stored : std::wstring();
}

void FolderPanel::NotifyOpenSelected(const std::wstring& path) const {
    if (path.empty() || hwnd_ == nullptr) {
        return;
    }
    HWND parent = ::GetParent(hwnd_);
    if (parent == nullptr) {
        return;
    }
    FolderNotify notification{};
    notification.hdr.hwndFrom = hwnd_;
    notification.hdr.idFrom = static_cast<UINT_PTR>(::GetDlgCtrlID(hwnd_));
    notification.hdr.code = kFolderNotifyCode;
    notification.action = FolderAction::OpenFile;
    notification.path = path;
    // 同步发：外壳会在这个通知里打开文件（与标签栏用同一条约定）。
    // 指针参数只在**同一进程**内传递，所以这里不涉及跨进程封送的问题。
    ::SendMessageW(parent, WM_NOTIFY, notification.hdr.idFrom,
                   reinterpret_cast<LPARAM>(&notification));
}

void FolderPanel::ActivateSelection() {
    // 选中目录时"展开/收起"而不是"打开"——面板只认"目录还是文件"这件事，
    // 不认任何文档概念
    const std::wstring path = SelectedPath();
    if (path.empty()) {
        return;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        const HTREEITEM item = reinterpret_cast<HTREEITEM>(
            ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_CARET, 0));
        if (item != nullptr) {
            ::SendMessageW(tree_, TVM_EXPAND, TVE_TOGGLE, reinterpret_cast<LPARAM>(item));
        }
        return;
    }
    NotifyOpenSelected(path);
}

LRESULT CALLBACK FolderPanel::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<FolderPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<FolderPanel*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT FolderPanel::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE: {
            if (tree_ != nullptr) {
                RECT client{};
                ::GetClientRect(hwnd_, &client);
                ::MoveWindow(tree_, 0, 0, client.right, client.bottom, TRUE);
            }
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header != nullptr && header->hwndFrom == tree_) {
                if (header->code == TVN_ITEMEXPANDEDW) {
                    // 展开时按需把没读过的子目录补上？本版一次性读完（深度上限以内），
                    // 所以这里只需要重画
                    return 0;
                }
                if (header->code == NM_DBLCLK || header->code == NM_RETURN) {
                    ActivateSelection();
                    return 0;
                }
            }
            break;
        }
        case WM_DESTROY: {
            // lParam 上挂的路径字符串要一起清掉，否则每次刷新都漏一批
            if (tree_ != nullptr) {
                const HTREEITEM root = reinterpret_cast<HTREEITEM>(
                    ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_ROOT, 0));
                if (root != nullptr) {
                    FreeItems(root);
                }
            }
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::shell
