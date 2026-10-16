#include "shell/TabBar.h"

#include "shell/ChromeStyle.h"

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>

namespace npcc::shell {
namespace {

int DpiScaleFor(HWND hwnd, int value) {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr) {
            const UINT actual = getDpi(hwnd);
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return ::MulDiv(value, static_cast<int>(dpi), 96);
}

}  // namespace

bool TabBar::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &TabBar::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // 全部自绘
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0;
}

bool TabBar::Create(HWND parent, int controlId) {
    hwnd_ = ::CreateWindowExW(0, kClassName, L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                              0, 0, 0, 0, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

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
    return true;
}

void TabBar::Destroy() {
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (hwnd_ != nullptr) {
        if (::IsWindow(hwnd_) != 0) {
            ::DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK TabBar::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    TabBar* self = reinterpret_cast<TabBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<TabBar*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

int TabBar::Scale(int value) const {
    return DpiScaleFor(hwnd_, value);
}

int TabBar::Height() const {
    RebuildMetrics();
    return tabHeight_;
}

void TabBar::RebuildMetrics() const {
    tabHeight_ = Scale(30);
    paddingX_ = Scale(10);
    minTabWidth_ = Scale(96);
    maxTabWidth_ = Scale(240);
    closeBoxSize_ = Scale(16);

    tabWidths_.assign(items_.size(), minTabWidth_);
    if (hwnd_ == nullptr) {
        return;
    }
    const HDC dc = ::GetDC(hwnd_);
    if (dc == nullptr) {
        return;
    }
    const HGDIOBJ previous = ::SelectObject(dc, font_);
    for (size_t i = 0; i < items_.size(); ++i) {
        // 标题可能很长，测量时截断，避免为超长文件名撑出巨宽标签
        std::wstring sample = items_[i].title;
        if (sample.size() > 40) {
            sample.resize(40);
        }
        SIZE size{};
        if (::GetTextExtentPoint32W(dc, sample.c_str(), static_cast<int>(sample.size()), &size) != 0) {
            const int wanted = size.cx + paddingX_ * 2 + closeBoxSize_ + Scale(8);
            tabWidths_[i] = std::clamp(wanted, minTabWidth_, maxTabWidth_);
        }
    }
    ::SelectObject(dc, previous);
    ::ReleaseDC(hwnd_, dc);
}

int TabBar::TabWidth(size_t index) const {
    if (tabWidths_.size() != items_.size()) {
        RebuildMetrics();
    }
    return index < tabWidths_.size() ? tabWidths_[index] : minTabWidth_;
}

int TabBar::TotalWidth() const {
    int total = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        total += TabWidth(i);
    }
    return total;
}

RECT TabBar::ItemRect(size_t index) const {
    int x = -scrollOffset_;
    for (size_t i = 0; i < index && i < items_.size(); ++i) {
        x += TabWidth(i);
    }
    RECT rc{};
    rc.left = x;
    rc.top = 0;
    rc.right = x + TabWidth(index);
    rc.bottom = tabHeight_;
    return rc;
}

int TabBar::IndexFromPoint(POINT point) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        const RECT rc = ItemRect(i);
        if (point.x >= rc.left && point.x < rc.right && point.y >= rc.top && point.y < rc.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TabBar::CloseHitIndex(POINT point) const {
    const int index = IndexFromPoint(point);
    if (index < 0) {
        return -1;
    }
    const RECT rc = ItemRect(static_cast<size_t>(index));
    const int boxLeft = rc.right - paddingX_ / 2 - closeBoxSize_;
    if (point.x >= boxLeft && point.x <= rc.right) {
        return index;
    }
    return -1;
}

void TabBar::EnsureVisible(size_t index) {
    if (index >= items_.size() || hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int left = ItemRect(index).left;
    const int right = ItemRect(index).right;

    if (left < 0) {
        scrollOffset_ += left;
    } else if (right > width) {
        scrollOffset_ += right - width;
    }
    const int maxOffset = std::max(0, TotalWidth() - width);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxOffset);
}

void TabBar::EnsureActiveVisible() {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == active_) {
            EnsureVisible(i);
            break;
        }
    }
}

void TabBar::ScrollBy(int delta) {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int maxOffset =
        std::max(0, TotalWidth() - static_cast<int>(client.right - client.left));
    const int next = std::clamp(scrollOffset_ + delta, 0, maxOffset);
    if (next != scrollOffset_) {
        scrollOffset_ = next;
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void TabBar::SetItems(std::vector<TabItem> items, npcc::core::BufferId active) {
    items_ = std::move(items);
    active_ = active;
    dragIndex_ = -1;
    dropIndex_ = -1;
    hotIndex_ = -1;
    hotCloseIndex_ = -1;
    dragging_ = false;
    RebuildMetrics();
    EnsureActiveVisible();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void TabBar::SetActive(npcc::core::BufferId active) {
    active_ = active;
    EnsureActiveVisible();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void TabBar::SetDark(bool dark) {
    if (dark_ == dark) {
        return;
    }
    dark_ = dark;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);   // 整块重画（背景色变了）
    }
}

void TabBar::SetEyeCare(bool eyeCare) {
    if (eyeCare_ == eyeCare) {
        return;
    }
    eyeCare_ = eyeCare;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void TabBar::Notify(TabAction action, npcc::core::BufferId source,
                    npcc::core::BufferId target) const {
    if (hwnd_ == nullptr) {
        return;
    }
    TabBarNotify notification{};
    notification.hdr.hwndFrom = hwnd_;
    notification.hdr.idFrom = static_cast<UINT_PTR>(::GetDlgCtrlID(hwnd_));
    notification.hdr.code = kTabBarNotifyCode;
    notification.action = action;
    notification.source = source;
    notification.target = target;
    // 同步投递：调用方需要立刻拿到结果，且回调只在 UI 线程发生
    ::SendMessageW(::GetParent(hwnd_), WM_NOTIFY, notification.hdr.idFrom,
                   reinterpret_cast<LPARAM>(&notification));
}

void TabBar::ShowContextMenu(POINT screenPoint, size_t index) {
    const TabItem& item = items_[index];

    enum : UINT { kClose = 1, kCloseOthers, kCloseRight, kCloseAll, kCopyPath };

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING, kClose, L"关闭");
    ::AppendMenuW(menu, MF_STRING, kCloseOthers, L"关闭其他文档");
    ::AppendMenuW(menu, MF_STRING, kCloseRight, L"关闭右侧文档");
    ::AppendMenuW(menu, MF_STRING, kCloseAll, L"关闭全部文档");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (item.hasPath ? 0u : MF_GRAYED), kCopyPath, L"复制完整路径");

    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    ::DestroyMenu(menu);

    switch (command) {
        case kClose:       Notify(TabAction::Close, item.id); break;
        case kCloseOthers: Notify(TabAction::CloseOthers, item.id); break;
        case kCloseRight:  Notify(TabAction::CloseRight, item.id); break;
        case kCloseAll:    Notify(TabAction::CloseAll, item.id); break;
        case kCopyPath:    Notify(TabAction::CopyPath, item.id); break;
        default: break;
    }
}

void TabBar::Paint(HDC target, const RECT& client) {
    const HGDIOBJ previousFont = ::SelectObject(target, font_);
    ::SetBkMode(target, TRANSPARENT);

    const ChromeColours colours = ChromeColoursFor(dark_, eyeCare_);
    const HBRUSH background = ::CreateSolidBrush(colours.strip);
    ::FillRect(target, &client, background);
    ::DeleteObject(background);

    const int radius = Scale(7);
    const int underline = Scale(3);
    const int textPadding = paddingX_;
    int activeLeft = 0;
    int activeRight = 0;
    bool hasActive = false;

    // (1) 页签底色：当前标签是"和编辑区同色"的圆角页签——底部多画 radius，
    //     两个底角落到窗口外，看上去只有上边是圆的；悬停标签铺一层浅底。
    //     拖动中的标签不铺底，留出"空位"。
    for (size_t i = 0; i < items_.size(); ++i) {
        const RECT rc = ItemRect(i);
        if (rc.right <= client.left || rc.left >= client.right) {
            continue;
        }
        const bool isActive = (items_[i].id == active_);
        const bool isHot = (static_cast<int>(i) == hotIndex_);
        if (isActive) {
            hasActive = true;
            activeLeft = std::max<int>(rc.left, client.left);
            activeRight = std::min<int>(rc.right, client.right);
            RECT tab = rc;
            tab.bottom += radius;
            FillRoundedRect(target, tab, radius, colours.tabActive);
        } else if (isHot && !(dragging_ && static_cast<int>(i) == dragIndex_)) {
            RECT tab = rc;
            tab.bottom += radius;
            FillRoundedRect(target, tab, radius, colours.tabHover);
        }
    }

    // (2) 底边细线：当前标签脚下让开一段——"页签和编辑区连成一片"是这一眼最要紧的。
    //     用 FillRect 而不是画线笔，细线宽度才能跟着 DPI 走（200% 下还是"一条线"）。
    const int hairline = Scale(1);
    const HBRUSH lineBrush = ::CreateSolidBrush(colours.line);
    if (!hasActive || activeLeft > client.left) {
        RECT leftLine{client.left, client.bottom - hairline,
                      hasActive ? activeLeft : client.right, client.bottom};
        ::FillRect(target, &leftLine, lineBrush);
    }
    if (hasActive && activeRight < client.right) {
        RECT rightLine{activeRight, client.bottom - hairline, client.right, client.bottom};
        ::FillRect(target, &rightLine, lineBrush);
    }
    ::DeleteObject(lineBrush);

    // (3) 当前标签的强调色下划线（Windows 11 那一条）
    if (hasActive && activeRight > activeLeft) {
        RECT mark{activeLeft, client.bottom - underline, activeRight, client.bottom};
        const HBRUSH brush = ::CreateSolidBrush(colours.accent);
        ::FillRect(target, &mark, brush);
        ::DeleteObject(brush);
    }

    // (4) 标题与关闭按钮
    for (size_t i = 0; i < items_.size(); ++i) {
        const RECT rc = ItemRect(i);
        if (rc.right <= client.left || rc.left >= client.right) {
            continue;
        }
        if (dragging_ && static_cast<int>(i) == dragIndex_) {
            continue;   // 正在拖动的那一项画成"空位"
        }
        const bool isActive = (items_[i].id == active_);
        ::SetTextColor(target, isActive ? colours.text : colours.textMuted);

        RECT textRect = rc;
        textRect.left += textPadding;
        textRect.right -= (textPadding / 2 + closeBoxSize_);
        if (textRect.right > textRect.left) {
            RECT measured = textRect;
            ::DrawTextW(target, items_[i].title.c_str(), -1, &measured,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | DT_CALCRECT);

            // 未保存标记：标题末尾一颗强调色小圆点；量下来放不下就退回字符标记
            const int closeGuard = rc.right - paddingX_ / 2 - closeBoxSize_;
            const bool drawDot = items_[i].modified && measured.right + Scale(9) < closeGuard;
            std::wstring label = items_[i].title;
            if (items_[i].modified && !drawDot) {
                label += L" \u25cf";
            }
            ::DrawTextW(target, label.c_str(), -1, &textRect,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (drawDot) {
                const int dot = Scale(3);
                const int dotX = measured.right + Scale(5);
                const int dotY = rc.top + tabHeight_ / 2;
                const HGDIOBJ oldPen = ::SelectObject(target, ::GetStockObject(NULL_PEN));
                const HGDIOBJ oldBrush = ::SelectObject(target, ::GetStockObject(DC_BRUSH));
                ::SetDCBrushColor(target, colours.accent);
                ::Ellipse(target, dotX, dotY - dot, dotX + 2 * dot, dotY + dot);
                ::SelectObject(target, oldBrush);
                ::SelectObject(target, oldPen);
            }
        }

        // 关闭按钮：悬停画一个软色圆底（不再是整块反色）
        const int boxSize = closeBoxSize_;
        const int boxLeft = rc.right - paddingX_ / 2 - boxSize;
        RECT closeRect{boxLeft, rc.top + (tabHeight_ - boxSize) / 2,
                       boxLeft + boxSize, rc.top + (tabHeight_ + boxSize) / 2};
        const bool closeHot = (static_cast<int>(i) == hotCloseIndex_);
        if (closeHot) {
            RECT bubble = closeRect;
            ::InflateRect(&bubble, Scale(2), Scale(2));
            FillRoundedRect(target, bubble, (bubble.right - bubble.left) / 2, colours.dangerSoft);
        }
        ::SetTextColor(target, closeHot ? colours.danger : colours.textMuted);
        ::DrawTextW(target, L"\u00d7", -1, &closeRect,
                    DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    // 拖拽落点指示线
    if (dragging_ && dropIndex_ >= 0) {
        const int markerX = (static_cast<size_t>(dropIndex_) < items_.size())
                                ? ItemRect(static_cast<size_t>(dropIndex_)).left
                                : ItemRect(items_.size() - 1).right;
        const HPEN marker = ::CreatePen(PS_SOLID, Scale(2), colours.accent);
        const HGDIOBJ oldPen = ::SelectObject(target, marker);
        ::MoveToEx(target, markerX, Scale(2), nullptr);
        ::LineTo(target, markerX, tabHeight_ - Scale(2));
        ::SelectObject(target, oldPen);
        ::DeleteObject(marker);
    }

    ::SelectObject(target, previousFont);
}

LRESULT TabBar::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case kMsgActiveTabCenterX: {
            for (size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].id == active_) {
                    const RECT rc = ItemRect(i);
                    return static_cast<LRESULT>((rc.left + rc.right) / 2);
                }
            }
            return -1;
        }
        case WM_NCCREATE: {
            // 默认尺寸只作占位，实际高度由父窗口按 Height() 设定
            return TRUE;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC dc = ::BeginPaint(hwnd_, &ps);
            RECT client{};
            ::GetClientRect(hwnd_, &client);

            // 双缓冲，避免拖动与悬停时闪烁
            const HDC memory = ::CreateCompatibleDC(dc);
            const HBITMAP bitmap = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
            const HGDIOBJ oldBitmap = ::SelectObject(memory, bitmap);
            Paint(memory, client);
            ::BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            ::SelectObject(memory, oldBitmap);
            ::DeleteObject(bitmap);
            ::DeleteDC(memory);

            ::EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SIZE:
            EnsureActiveVisible();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (::GetCapture() == hwnd_ && dragIndex_ >= 0) {
                if (!dragging_) {
                    const int dx = point.x - dragOrigin_.x;
                    const int dy = point.y - dragOrigin_.y;
                    if (dx * dx + dy * dy > Scale(5) * Scale(5)) {
                        dragging_ = true;
                    }
                }
                if (dragging_) {
                    int drop = static_cast<int>(items_.size());
                    for (size_t i = 0; i < items_.size(); ++i) {
                        const RECT rc = ItemRect(i);
                        if (point.x < (rc.left + rc.right) / 2) {
                            drop = static_cast<int>(i);
                            break;
                        }
                    }
                    if (drop != dropIndex_) {
                        dropIndex_ = drop;
                        ::InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
                return 0;
            }

            const int hovered = IndexFromPoint(point);
            const int hoveredClose = CloseHitIndex(point);
            if (hovered != hotIndex_ || hoveredClose != hotCloseIndex_) {
                hotIndex_ = hovered;
                hotCloseIndex_ = hoveredClose;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hotIndex_ = -1;
            hotCloseIndex_ = -1;
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = IndexFromPoint(point);
            if (index < 0) {
                return 0;
            }
            if (CloseHitIndex(point) == index) {
                Notify(TabAction::Close, items_[static_cast<size_t>(index)].id);
                return 0;
            }
            dragIndex_ = index;
            dropIndex_ = -1;
            dragging_ = false;
            dragOrigin_ = point;
            ::SetCapture(hwnd_);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (::GetCapture() == hwnd_) {
                ::ReleaseCapture();
            }
            if (dragIndex_ >= 0) {
                const npcc::core::BufferId source = items_[static_cast<size_t>(dragIndex_)].id;
                if (dragging_ && dropIndex_ >= 0) {
                    // 落在自己原位置或紧邻位置视为没动
                    const bool noMove = (dropIndex_ == dragIndex_ || dropIndex_ == dragIndex_ + 1);
                    const npcc::core::BufferId target =
                        (static_cast<size_t>(dropIndex_) < items_.size())
                            ? items_[static_cast<size_t>(dropIndex_)].id
                            : npcc::core::kInvalidId;
                    if (!noMove) {
                        Notify(TabAction::Reorder, source, target);
                    }
                } else {
                    Notify(TabAction::Activate, source);
                }
            }
            dragIndex_ = -1;
            dropIndex_ = -1;
            dragging_ = false;
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = IndexFromPoint(point);
            if (index >= 0) {
                Notify(TabAction::Close, items_[static_cast<size_t>(index)].id);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = IndexFromPoint(point);
            if (index >= 0) {
                Notify(TabAction::Activate, items_[static_cast<size_t>(index)].id);
                POINT screen = point;
                ::ClientToScreen(hwnd_, &screen);
                ShowContextMenu(screen, static_cast<size_t>(index));
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ScrollBy(-delta / WHEEL_DELTA * Scale(60));
            return 0;
        }
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::shell
