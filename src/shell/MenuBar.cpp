#include "shell/MenuBar.h"

#include "shell/ChromeStyle.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace npcc::shell {
namespace {

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

bool MenuBar::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MenuBar::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool MenuBar::Create(HWND parent, int controlId) {
    if (hwnd_ != nullptr) {
        return true;
    }
    parent_ = parent;
    hwnd_ = ::CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                              0, 0, 0, 0, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }
    font_ = CreateChromeFont(DpiOf(hwnd_), 95, false);
    RebuildItems();
    return true;
}

void MenuBar::Destroy() {
    if (hwnd_ != nullptr && ::IsWindow(hwnd_) != 0) {
        ::DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

int MenuBar::Scale(int value) const {
    return ::MulDiv(value, DpiOf(hwnd_), 96);
}

int MenuBar::Height() const {
    return visible_ ? Scale(30) : 0;
}

void MenuBar::SetMenu(HMENU menu) {
    if (menu_ == menu) {
        return;
    }
    menu_ = menu;
    RebuildItems();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void MenuBar::SetTheme(bool dark, bool eyeCare) {
    if (dark_ == dark && eyeCare_ == eyeCare) {
        return;
    }
    dark_ = dark;
    eyeCare_ = eyeCare;
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void MenuBar::SetVisible(bool visible) {
    visible_ = visible;
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible_ ? SW_SHOW : SW_HIDE);
        if (visible_) {
            // 全屏期间菜单可能被 RebuildMenu 换过；那时控件高度是 0，按 0 算出来的
            // 命中矩形不能用。恢复显示时按当前高度重建一次。
            RebuildItems();
        }
        if (!visible_) {
            active_ = false;
            hot_ = -1;
            open_ = -1;
        }
    }
}

void MenuBar::Refresh() const {
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void MenuBar::RebuildItems() {
    items_.clear();
    focus_ = 0;
    hot_ = -1;
    if (menu_ == nullptr || hwnd_ == nullptr) {
        return;
    }
    const int count = ::GetMenuItemCount(menu_);
    if (count <= 0) {
        return;
    }
    const HDC dc = ::GetDC(hwnd_);
    if (dc == nullptr) {
        return;
    }
    const HGDIOBJ previous = (font_ != nullptr) ? ::SelectObject(dc, font_) : nullptr;
    int left = Scale(8);
    const int top = Scale(2);
    // 不能读 Height()：隐藏在全屏时它是 0，但菜单仍可能被语言/快捷键重建。
    // 命中矩形始终按正常高度算，恢复显示时才是对的。
    const int bottom = Scale(30) - Scale(2);
    for (int i = 0; i < count; ++i) {
        wchar_t buffer[256]{};
        const int length = ::GetMenuStringW(menu_, static_cast<UINT>(i), buffer, 256,
                                            MF_BYPOSITION);
        if (length <= 0) {
            continue;
        }
        std::wstring title(buffer, static_cast<std::size_t>(length));
        Item item;
        item.title = title;
        item.label = ChromeMenuLabel(title, &item.mnemonic);
        SIZE extent{};
        ::GetTextExtentPoint32W(dc, item.label.c_str(), static_cast<int>(item.label.size()),
                                &extent);
        const int width = extent.cx + Scale(24);
        item.rect = RECT{left, top, left + width, bottom};
        left += width;
        items_.push_back(std::move(item));
    }
    if (previous != nullptr) {
        ::SelectObject(dc, previous);
    }
    ::ReleaseDC(hwnd_, dc);
}

int MenuBar::HitTest(POINT point) const {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (::PtInRect(&items_[i].rect, point) != 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MenuBar::Paint(HDC dc) const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const ChromeColours colours = ChromeColoursFor(dark_, eyeCare_);
    const HBRUSH background = ::CreateSolidBrush(colours.menuBar);
    ::FillRect(dc, &client, background);
    ::DeleteObject(background);

    ::SetBkMode(dc, TRANSPARENT);
    if (font_ != nullptr) {
        ::SelectObject(dc, font_);
    }
    for (std::size_t i = 0; i < items_.size(); ++i) {
        DrawItem(dc, static_cast<int>(i));
    }

    const int hairline = Scale(1);
    RECT divider{0, client.bottom - hairline, client.right, client.bottom};
    const HBRUSH line = ::CreateSolidBrush(colours.line);
    ::FillRect(dc, &divider, line);
    ::DeleteObject(line);
}

void MenuBar::DrawItem(HDC dc, int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    const Item& item = items_[static_cast<std::size_t>(index)];
    const ChromeColours colours = ChromeColoursFor(dark_, eyeCare_);
    const bool isOpen = open_ == index;
    const bool isHot = hot_ == index;
    const bool isFocus = active_ && focus_ == index;
    if (isOpen || isHot || isFocus) {
        RECT fill = item.rect;
        ::InflateRect(&fill, -Scale(1), 0);
        FillRoundedRect(dc, fill, Scale(6),
                        isOpen ? colours.accentSoft : colours.buttonHot);
    }
    ::SetTextColor(dc, isOpen ? colours.accent : colours.text);
    RECT text = item.rect;
    const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_CENTER |
                       (active_ ? 0u : DT_NOPREFIX);
    ::DrawTextW(dc, active_ ? item.title.c_str() : item.label.c_str(), -1, &text, flags);
}

std::wstring MenuBar::VisibleText() const {
    std::wstring text;
    for (const Item& item : items_) {
        if (!text.empty()) {
            text += L" | ";
        }
        text += item.label;
    }
    return text;
}

void MenuBar::Activate() {
    if (hwnd_ == nullptr || !visible_ || items_.empty()) {
        return;
    }
    active_ = true;
    hot_ = -1;
    focus_ = std::max(0, std::min(focus_, static_cast<int>(items_.size()) - 1));
    ::SetFocus(hwnd_);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void MenuBar::Deactivate(bool focusParent) {
    active_ = false;
    hot_ = -1;
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    if (focusParent && parent_ != nullptr) {
        ::SetFocus(parent_);
    }
}

void MenuBar::OpenAt(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size()) || menu_ == nullptr) {
        return;
    }
    const HMENU popup = ::GetSubMenu(menu_, index);
    if (popup == nullptr) {
        return;
    }
    focus_ = index;
    open_ = index;
    active_ = true;
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    ::UpdateWindow(hwnd_);

    POINT point{items_[static_cast<std::size_t>(index)].rect.left, Height()};
    ::ClientToScreen(hwnd_, &point);
    const UINT command = ::TrackPopupMenuEx(
        popup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD,
        point.x, point.y, parent_, nullptr);
    open_ = -1;
    if (command != 0 && parent_ != nullptr) {
        ::PostMessageW(parent_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
    Deactivate(true);
}

LRESULT CALLBACK MenuBar::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MenuBar* self = reinterpret_cast<MenuBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MenuBar*>(create->lpCreateParams);
        if (self != nullptr) {
            self->hwnd_ = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT MenuBar::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd_, &paint);
            Paint(dc);
            ::EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            RebuildItems();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd_;
                ::TrackMouseEvent(&track);
                tracking_ = true;
            }
            const int index = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (index != hot_) {
                hot_ = index;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hot_ != -1) {
                hot_ = -1;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const int index = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (index >= 0) {
                ::SetFocus(hwnd_);
                OpenAt(index);
                return 0;
            }
            break;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            if (message == WM_KILLFOCUS && open_ < 0) {
                active_ = false;
                hot_ = -1;
            }
            ::InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_MENU) {
                Activate();
                return 0;
            }
            if (!active_) {
                break;
            }
            switch (wParam) {
                case VK_LEFT:
                    focus_ = (focus_ + static_cast<int>(items_.size()) - 1) %
                             static_cast<int>(items_.size());
                    ::InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case VK_RIGHT:
                    focus_ = (focus_ + 1) % static_cast<int>(items_.size());
                    ::InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                case VK_DOWN:
                case VK_RETURN:
                case VK_SPACE:
                    OpenAt(focus_);
                    return 0;
                case VK_ESCAPE:
                    Deactivate(true);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_CHAR:
        case WM_SYSCHAR: {
            if (!active_) {
                break;
            }
            const wchar_t wanted = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(wParam)));
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].mnemonic != L'\0' &&
                    std::towupper(static_cast<wint_t>(items_[i].mnemonic)) == wanted) {
                    OpenAt(static_cast<int>(i));
                    return 0;
                }
            }
            break;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_GETTEXTLENGTH: {
            return static_cast<LRESULT>(VisibleText().size());
        }
        case WM_GETTEXT: {
            const std::wstring text = VisibleText();
            const auto capacity = static_cast<std::size_t>(wParam);
            if (capacity > 0) {
                const std::size_t copied = std::min(capacity - 1, text.size());
                std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), text.c_str(), copied);
                reinterpret_cast<wchar_t*>(lParam)[copied] = L'\0';
            }
            return static_cast<LRESULT>(text.size());
        }
        case kMsgItemCount:
            return static_cast<LRESULT>(items_.size());
        case kMsgItemCenterX: {
            const int index = static_cast<int>(wParam);
            if (index < 0 || index >= static_cast<int>(items_.size())) {
                return -1;
            }
            const RECT& rect = items_[static_cast<std::size_t>(index)].rect;
            return (rect.left + rect.right) / 2;
        }
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            if (font_ != nullptr) {
                ::DeleteObject(font_);
                font_ = nullptr;
            }
            break;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::shell
