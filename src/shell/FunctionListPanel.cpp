#include "shell/FunctionListPanel.h"

#include <commctrl.h>

#include <cwchar>
#include <functional>
#include <utility>


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

bool FunctionListPanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &FunctionListPanel::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int FunctionListPanel::Scale(int value) const {
    return ::MulDiv(value, DpiOf(hwnd_), 96);
}

bool FunctionListPanel::Create(HWND parent, int controlId) {
    if (hwnd_ != nullptr) {
        return true;
    }
    hwnd_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, kClassName, L"",
                              WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
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

void FunctionListPanel::Destroy() {
    if (hwnd_ != nullptr) {
        if (::IsWindow(hwnd_) != 0) {
            ::DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }
    tree_ = nullptr;
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

void FunctionListPanel::SetVisible(bool visible) {
    visible_ = visible;
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible_ ? SW_SHOW : SW_HIDE);
    }
}

int FunctionListPanel::Width() const {
    return visible_ ? Scale(220) : 0;
}

void FunctionListPanel::Resize(const RECT& rect) const {
    if (hwnd_ == nullptr || tree_ == nullptr) {
        return;
    }
    ::MoveWindow(hwnd_, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    ::MoveWindow(tree_, 0, 0, client.right, client.bottom, TRUE);
}

void FunctionListPanel::FreeItems(HTREEITEM item) {
    if (tree_ == nullptr || item == nullptr) {
        return;
    }
    HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(
        tree_, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(item)));
    while (child != nullptr) {
        HTREEITEM next = reinterpret_cast<HTREEITEM>(::SendMessageW(
            tree_, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
        FreeItems(child);
        child = next;
    }
    TVITEMW info{};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    if (::SendMessageW(tree_, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&info)) != 0 &&
        info.lParam != 0) {
        delete reinterpret_cast<int*>(info.lParam);
        info.lParam = 0;
        ::SendMessageW(tree_, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&info));
    }
}

void FunctionListPanel::SetSymbols(const std::vector<npcc::core::FunctionSymbol>& symbols) {
    if (tree_ == nullptr) {
        return;
    }
    const HTREEITEM root = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    if (root != nullptr) {
        FreeItems(root);
    }
    ::SendMessageW(tree_, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));
    if (symbols.empty()) {
        return;
    }
    // depth 决定挂在谁下面：depth 0 是顶层，depth N 挂到"最近的 depth N-1"上。
    // 用栈保存当前这条链，遇到更浅或同层的就把它弹出——跳级（提取器给 2 但前面只有 0）
    // 时自然落到最近的那一层，不至于丢掉节点
    std::vector<std::pair<int, HTREEITEM>> stack;
    for (const npcc::core::FunctionSymbol& symbol : symbols) {
        const int depth = std::max(0, symbol.depth);
        while (!stack.empty() && stack.back().first >= depth) {
            stack.pop_back();
        }
        const HTREEITEM parent = stack.empty() ? TVI_ROOT : stack.back().second;
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        // 显示成"名字    (第 N 行)"：省得用户点进去才知道在哪
        std::wstring text = symbol.name + L"    (" + std::to_wstring(symbol.line + 1) + L")";
        insert.item.pszText = const_cast<wchar_t*>(text.c_str());
        auto* stored = new int(symbol.line);
        insert.item.lParam = reinterpret_cast<LPARAM>(stored);
        const HTREEITEM item = reinterpret_cast<HTREEITEM>(
            ::SendMessageW(tree_, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
        if (item == nullptr) {
            delete stored;
            continue;
        }
        stack.push_back({depth, item});
    }
    // 全部展开：函数列表就那么几十项，收着只会让人多点几下
    std::function<void(HTREEITEM)> expandAll = [&](HTREEITEM item) {
        ::SendMessageW(tree_, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(item));
        for (HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(
                 tree_, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(item)));
             child != nullptr;
             child = reinterpret_cast<HTREEITEM>(::SendMessageW(
                 tree_, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)))) {
            expandAll(child);
        }
    };
    for (HTREEITEM item = reinterpret_cast<HTREEITEM>(::SendMessageW(tree_, TVM_GETNEXTITEM,
                                                                    TVGN_ROOT, 0));
         item != nullptr;
         item = reinterpret_cast<HTREEITEM>(::SendMessageW(
             tree_, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(item)))) {
        expandAll(item);
    }
}

int FunctionListPanel::SelectedLine() const {
    if (tree_ == nullptr) {
        return -1;
    }
    const HTREEITEM item = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree_, TVM_GETNEXTITEM, TVGN_CARET, 0));
    if (item == nullptr) {
        return -1;
    }
    TVITEMW info{};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    if (::SendMessageW(tree_, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&info)) == 0) {
        return -1;
    }
    const auto* line = reinterpret_cast<const int*>(info.lParam);
    return line != nullptr ? *line : -1;
}

void FunctionListPanel::ActivateSelection() {
    const int line = SelectedLine();
    if (line < 0 || hwnd_ == nullptr) {
        return;
    }
    HWND parent = ::GetParent(hwnd_);
    if (parent == nullptr) {
        return;
    }
    FunctionNotify notification{};
    notification.hdr.hwndFrom = hwnd_;
    notification.hdr.idFrom = static_cast<UINT_PTR>(::GetDlgCtrlID(hwnd_));
    notification.hdr.code = kFunctionNotifyCode;
    notification.action = FunctionAction::JumpToLine;
    notification.line = line;
    ::SendMessageW(parent, WM_NOTIFY, notification.hdr.idFrom,
                   reinterpret_cast<LPARAM>(&notification));
}

LRESULT CALLBACK FunctionListPanel::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam) {
    auto* self = reinterpret_cast<FunctionListPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<FunctionListPanel*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT FunctionListPanel::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
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
            if (header != nullptr && header->hwndFrom == tree_ &&
                (header->code == NM_DBLCLK || header->code == NM_RETURN)) {
                ActivateSelection();
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
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
