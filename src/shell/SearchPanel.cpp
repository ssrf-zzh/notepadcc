#include "shell/SearchPanel.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>

#include "infra/Log.h"
#include "search/TextBytes.h"

namespace npcc::shell {
namespace {

constexpr int kGap = 6;
constexpr int kRowHeight = 25;
constexpr int kListHeight = 170;
constexpr UINT kMsgUpdateScrollCorner = WM_APP + 41;
constexpr const wchar_t* kScrollCornerClass = L"notepadccSearchScrollCorner";

void SetListSubItem(HWND list, int item, int subItem, const std::wstring& text) {
    LVITEMW entry{};
    entry.mask = LVIF_TEXT;
    entry.iItem = item;
    entry.iSubItem = subItem;
    entry.pszText = const_cast<LPWSTR>(text.c_str());
    ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(item),
                   reinterpret_cast<LPARAM>(&entry));
}

}  // namespace

bool SearchPanel::Create(HWND parent) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SearchPanel::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // 背景由 WM_PAINT 按主题填，避免深色 / 护眼模式被系统灰盖住
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"notepadccSearchPanel";
    ::RegisterClassExW(&wc);

    WNDCLASSEXW corner{};
    corner.cbSize = sizeof(corner);
    corner.lpfnWndProc = &SearchPanel::CornerWndProc;
    corner.hInstance = ::GetModuleHandleW(nullptr);
    corner.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    corner.lpszClassName = kScrollCornerClass;
    ::RegisterClassExW(&corner);

    hwnd_ = ::CreateWindowExW(0, L"notepadccSearchPanel", L"",
                              WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr,
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

    // 用 cue banner 当占位提示，比在每个输入框前面摆一个 STATIC 省一半控件
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelFind, L"查找内容");
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelDir, L"搜索目录（留空则用当前文件所在目录）");
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelFilter, L"文件过滤器：*.txt;*.cpp;!node_modules");
    MakeControl(L"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                0, kSearchPanelReplaceEdit, L"替换为");

    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                kSearchPanelCase, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                kSearchPanelWord, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                kSearchPanelRegex, nullptr);
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelCase), L"区分大小写");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelWord), L"全字匹配");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelRegex), L"正则");

    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelStart, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelStop, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelReplacePreview, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelApply, nullptr);
    MakeControl(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kSearchPanelClose, nullptr);
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelStart), L"在文件中查找");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelStop), L"停止");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelReplacePreview), L"替换…");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelApply), L"应用");
    ::SetWindowTextW(::GetDlgItem(hwnd_, kSearchPanelClose), L"关闭");

    list_ = MakeControl(WC_LISTVIEWW,
                        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | WS_CLIPSIBLINGS |
                            LVS_REPORT |
                            LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                        0, kSearchPanelList, nullptr);
    // 系统列表的滚动条交叉角不跟 DarkMode_Explorer 走，会留一块系统白。
    // 用专用自绘子窗口盖住它，比改系统非客户区的绘制可靠；列表与覆盖层都要
    // WS_CLIPSIBLINGS，否则列表重画非客户区时会盖过同级覆盖层。
    scrollCorner_ = ::CreateWindowExW(
        0, kScrollCornerClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchPanelScrollCorner)),
        ::GetModuleHandleW(nullptr), this);
    status_ = MakeControl(L"STATIC",
                          WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_CENTERIMAGE,
                          0, kSearchPanelStatus, nullptr);

    ApplyFont();
    SetupColumns();
    Layout();
    SetTheme(dark_, eyeCare_);
    SetStatus(L"就绪");
    SetApplyEnabled(false);
    return true;
}

void SearchPanel::Destroy() {
    if (hwnd_ != nullptr) {
        if (::IsWindow(hwnd_) != 0) {
            ::DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
    }
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (background_ != nullptr) {
        ::DeleteObject(background_);
        background_ = nullptr;
    }
    if (field_ != nullptr) {
        ::DeleteObject(field_);
        field_ = nullptr;
    }
    rows_.clear();
}

HWND SearchPanel::MakeControl(const wchar_t* className, DWORD style, DWORD exStyle, int id,
                              const wchar_t* cue) {
    const HWND control = ::CreateWindowExW(exStyle, className, L"", style, 0, 0, 10, 10, hwnd_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           ::GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr && cue != nullptr) {
        ::SendMessageW(control, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    }
    return control;
}

void SearchPanel::ApplyFont() {
    // 与查找栏 / 菜单栏 / 状态栏同一条字体链
    font_ = CreateChromeFont(static_cast<int>(Scale(96)), 95, false);
    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    InstallButtonSubclasses();
    if (list_ != nullptr) {
        ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ::SetWindowSubclass(list_, &SearchPanel::ListSubclass, 6000,
                            reinterpret_cast<DWORD_PTR>(this));
    }
}

ChromeColours SearchPanel::Theme() const {
    return ChromeColoursFor(dark_, eyeCare_);
}

void SearchPanel::SetTheme(bool dark, bool eyeCare) {
    dark_ = dark;
    eyeCare_ = eyeCare;
    UpdateThemeResources();
    if (hwnd_ == nullptr) {
        return;
    }
    ApplyThemeToControls();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    ::EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM) -> BOOL {
            ::InvalidateRect(child, nullptr, TRUE);
            return TRUE;
        },
        0);
}

void SearchPanel::UpdateThemeResources() {
    if (background_ != nullptr) {
        ::DeleteObject(background_);
        background_ = nullptr;
    }
    if (field_ != nullptr) {
        ::DeleteObject(field_);
        field_ = nullptr;
    }
    if (hwnd_ != nullptr) {
        const ChromeColours colours = Theme();
        background_ = ::CreateSolidBrush(colours.strip);
        field_ = ::CreateSolidBrush(colours.tabActive);
    }
}

void SearchPanel::ApplyThemeToControls() {
    if (hwnd_ == nullptr) {
        return;
    }
    const ChromeColours colours = Theme();
    ::EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM param) -> BOOL {
            const auto* self = reinterpret_cast<const SearchPanel*>(param);
            wchar_t className[32]{};
            ::GetClassNameW(child, className, 32);
            if (::_wcsicmp(className, L"Edit") == 0) {
                ApplyControlTheme(child, self->dark_, L"DarkMode_CFD");
            } else if (::_wcsicmp(className, L"SysListView32") == 0) {
                ApplyControlTheme(child, self->dark_, L"DarkMode_Explorer");
                const HWND header = ListView_GetHeader(child);
                if (header != nullptr) {
                    ApplyControlTheme(header, self->dark_, L"DarkMode_Explorer");
                    ::InvalidateRect(header, nullptr, TRUE);
                }
            } else if (::_wcsicmp(className, L"Button") == 0) {
                ::InvalidateRect(child, nullptr, TRUE);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));

    if (list_ != nullptr) {
        ::SendMessageW(list_, LVM_SETBKCOLOR, 0, colours.tabActive);
        ::SendMessageW(list_, LVM_SETTEXTBKCOLOR, 0, colours.tabActive);
        ::SendMessageW(list_, LVM_SETTEXTCOLOR, 0, colours.text);
    }
}

void SearchPanel::InstallButtonSubclasses() {
    if (hwnd_ == nullptr) {
        return;
    }
    ::EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t className[32]{};
            ::GetClassNameW(child, className, 32);
            if (::_wcsicmp(className, L"Button") == 0) {
                ::SetWindowSubclass(child, &SearchPanel::ButtonSubclass,
                                    static_cast<UINT_PTR>(5000 + ::GetDlgCtrlID(child)), param);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
}

void SearchPanel::PaintBackground(HDC dc) const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const HBRUSH brush = background_ != nullptr ? background_ : ::GetSysColorBrush(COLOR_WINDOW);
    ::FillRect(dc, &client, brush);
}

void SearchPanel::PaintScrollCorner(HDC dc) const {
    if (scrollCorner_ == nullptr || dc == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(scrollCorner_, &client);
    // 这里必须自己填：Static 在 PrintWindow / PRF_CLIENT 下会退回系统灰，
    // 真实屏幕与截图就成了两种颜色。自绘后两条路径都取同一份主题色。
    const HBRUSH brush = field_ != nullptr ? field_ : ::GetSysColorBrush(COLOR_WINDOW);
    ::FillRect(dc, &client, brush);
}

LRESULT CALLBACK SearchPanel::CornerWndProc(HWND hwnd, UINT message, WPARAM wParam,
                                            LPARAM lParam) {
    auto* self = reinterpret_cast<SearchPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SearchPanel*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd, &paint);
            self->PaintScrollCorner(dc);
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
        case WM_PRINT:
            self->PaintScrollCorner(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

void SearchPanel::PaintControl(HWND control, HDC dc) const {
    if (control == nullptr || dc == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(control, &client);
    wchar_t text[256]{};
    ::GetWindowTextW(control, text, 256);

    const LONG_PTR style = ::GetWindowLongPtrW(control, GWL_STYLE);
    const bool checkbox = (style & BS_TYPEMASK) == BS_AUTOCHECKBOX ||
                          (style & BS_TYPEMASK) == BS_CHECKBOX ||
                          (style & BS_TYPEMASK) == BS_AUTO3STATE ||
                          (style & BS_TYPEMASK) == BS_3STATE;
    const bool enabled = ::IsWindowEnabled(control) != 0;
    const bool focused = ::GetFocus() == control;
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const bool hovered = ::WindowFromPoint(cursor) == control;
    const DWORD state = static_cast<DWORD>(::SendMessageW(control, BM_GETSTATE, 0, 0));
    const bool pressed = (state & BST_PUSHED) != 0;
    const bool checked = ::SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int controlId = ::GetDlgCtrlID(control);
    const bool primary = controlId == kSearchPanelStart;
    const ChromeColours colours = Theme();
    const int dpi = static_cast<int>(Scale(96));

    if (checkbox) {
        DrawChromeCheckBox(dc, client, text, font_, colours, dpi, dark_, checked, hovered,
                           focused, enabled);
    } else {
        DrawChromePushButton(dc, client, text, font_, colours, dpi, dark_, primary, hovered,
                             pressed, focused, enabled);
    }
}

LRESULT CALLBACK SearchPanel::ButtonSubclass(HWND hwnd, UINT message, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<SearchPanel*>(ref);
    return self != nullptr ? self->ButtonSubclassProc(hwnd, message, wParam, lParam)
                           : ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT SearchPanel::ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                        LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd, &paint);
            PaintControl(hwnd, dc);
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintControl(hwnd, reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            ::TrackMouseEvent(&track);
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case WM_MOUSELEAVE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case WM_SETTEXT:
        case WM_UPDATEUISTATE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK SearchPanel::ListSubclass(HWND hwnd, UINT message, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<SearchPanel*>(ref);
    if (self != nullptr) {
        return self->ListSubclassProc(hwnd, message, wParam, lParam);
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

void SearchPanel::UpdateScrollCorner() {
    scrollCornerPending_ = false;
    if (scrollCorner_ == nullptr || list_ == nullptr) {
        return;
    }
    SCROLLINFO vertical{};
    vertical.cbSize = sizeof(vertical);
    vertical.fMask = SIF_RANGE | SIF_PAGE;
    SCROLLINFO horizontal{};
    horizontal.cbSize = sizeof(horizontal);
    horizontal.fMask = SIF_RANGE | SIF_PAGE;
    const bool verticalShown =
        ::GetScrollInfo(list_, SB_VERT, &vertical) != 0 &&
        vertical.nMax - vertical.nMin + 1 > static_cast<int>(vertical.nPage);
    const bool horizontalShown =
        ::GetScrollInfo(list_, SB_HORZ, &horizontal) != 0 &&
        horizontal.nMax - horizontal.nMin + 1 > static_cast<int>(horizontal.nPage);
    if (!verticalShown || !horizontalShown) {
        ::ShowWindow(scrollCorner_, SW_HIDE);
        return;
    }
    RECT area{};
    ::GetWindowRect(list_, &area);
    ::MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&area), 2);
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    const int scrollWidth = ::GetSystemMetrics(SM_CXVSCROLL);
    const int scrollHeight = ::GetSystemMetrics(SM_CYHSCROLL);
    ::SetWindowPos(scrollCorner_, HWND_TOP, area.left + width - scrollWidth,
                   area.top + height - scrollHeight, scrollWidth, scrollHeight,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void SearchPanel::ScheduleScrollCorner() {
    if (hwnd_ == nullptr || scrollCornerPending_) {
        return;
    }
    scrollCornerPending_ = true;
    ::PostMessageW(hwnd_, kMsgUpdateScrollCorner, 0, 0);
}

LRESULT SearchPanel::ListSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SIZE || message == WM_HSCROLL || message == WM_VSCROLL ||
        message == WM_MOUSEWHEEL) {
        const LRESULT result = ::DefSubclassProc(hwnd, message, wParam, lParam);
        ScheduleScrollCorner();
        return result;
    }
    // 表头的 NM_CUSTOMDRAW 只会送到列表视图本身，不会再到搜索面板。
    // 在这里拦一次，深色主题下表头才不会留一条系统白条。
    if (message == WM_NOTIFY) {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header != nullptr && header->code == NM_CUSTOMDRAW &&
            header->hwndFrom == ListView_GetHeader(hwnd)) {
            const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                RECT client{};
                ::GetClientRect(header->hwndFrom, &client);
                const HBRUSH brush = background_ != nullptr
                                         ? background_
                                         : ::GetSysColorBrush(COLOR_BTNFACE);
                ::FillRect(draw->hdc, &client, brush);
                return CDRF_NOTIFYITEMDRAW;
            }
            // 表头给每个子项发的是 CDDS_ITEMPREPAINT | CDDS_SUBITEM；原来用
            // == CDDS_ITEMPREPAINT 判断，永远进不来，系统白底就留了下来。
            if ((draw->dwDrawStage & CDDS_ITEMPREPAINT) == CDDS_ITEMPREPAINT) {
                const HBRUSH brush = background_ != nullptr
                                         ? background_
                                         : ::GetSysColorBrush(COLOR_BTNFACE);
                ::FillRect(draw->hdc, &draw->rc, brush);
                RECT line = draw->rc;
                line.top = line.bottom - std::max(1, Scale(1));
                const HBRUSH lineBrush = ::CreateSolidBrush(Theme().line);
                ::FillRect(draw->hdc, &line, lineBrush);
                ::DeleteObject(lineBrush);
                line = draw->rc;
                line.left = line.right - std::max(1, Scale(1));
                const HBRUSH dividerBrush = ::CreateSolidBrush(Theme().line);
                ::FillRect(draw->hdc, &line, dividerBrush);
                ::DeleteObject(dividerBrush);

                wchar_t text[256]{};
                HDITEMW item{};
                item.mask = HDI_TEXT;
                item.pszText = text;
                item.cchTextMax = static_cast<int>(std::size(text));
                if (::SendMessageW(header->hwndFrom, HDM_GETITEMW, draw->dwItemSpec,
                                   reinterpret_cast<LPARAM>(&item)) != 0) {
                    const HGDIOBJ oldFont = font_ != nullptr
                                                ? ::SelectObject(draw->hdc, font_)
                                                : nullptr;
                    ::SetBkMode(draw->hdc, TRANSPARENT);
                    ::SetTextColor(draw->hdc, Theme().text);
                    RECT textRect = draw->rc;
                    ::InflateRect(&textRect, -Scale(6), 0);
                    ::DrawTextW(draw->hdc, text, -1, &textRect,
                                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX |
                                    DT_END_ELLIPSIS);
                    if (oldFont != nullptr) {
                        ::SelectObject(draw->hdc, oldFont);
                    }
                }
                ::SetTextColor(draw->hdc, Theme().text);
                ::SetBkMode(draw->hdc, TRANSPARENT);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

void SearchPanel::SetupColumns() {
    if (list_ == nullptr) {
        return;
    }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.cx = Scale(320);
    column.pszText = const_cast<LPWSTR>(L"文件");
    ::SendMessageW(list_, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    column.cx = Scale(60);
    column.pszText = const_cast<LPWSTR>(L"行");
    ::SendMessageW(list_, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
    column.cx = Scale(520);
    column.pszText = const_cast<LPWSTR>(L"内容");
    ::SendMessageW(list_, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
}

int SearchPanel::Scale(int value) const {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr && hwnd_ != nullptr) {
            const UINT actual = getDpi(::GetParent(hwnd_));
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return ::MulDiv(value, static_cast<int>(dpi), 96);
}

int SearchPanel::Height() const {
    if (hwnd_ == nullptr) {
        return 0;
    }
    RECT parent{};
    ::GetClientRect(::GetParent(hwnd_), &parent);
    int width = parent.right - parent.left;
    if (width <= 0) {
        RECT own{};
        ::GetClientRect(hwnd_, &own);
        width = own.right - own.left;
    }
    int bottom = 0;
    for (const Placement& placement : BuildPlacements(width)) {
        bottom = std::max(bottom, static_cast<int>(placement.rect.bottom));
    }
    return bottom + Scale(kListHeight) + Scale(kGap) * 2;
}

std::vector<SearchPanel::Placement> SearchPanel::BuildPlacements(int width) const {
    std::vector<Placement> placements;
    if (hwnd_ == nullptr || width <= 0) {
        return placements;
    }
    const int gap = Scale(kGap);
    const int availableWidth = width - gap * 2;
    const bool compact = availableWidth < Scale(560);
    const int itemGap = compact ? Scale(4) : Scale(6);
    const int rowHeight = Scale(kRowHeight);
    // 中文标签需要按字形宽度留位：「区分大小写」/「匹配换行」约 5 个字，
    // 「在文件中查找」6 个字。只按英文按钮的直觉取 88 会在 960 宽时画出省略号。
    const int checkWidth = Scale(104);
    const int buttonWidth = Scale(compact ? 110 : 116);
    const int smallButton = Scale(compact ? 48 : 60);
    const int replaceButtonWidth = Scale(compact ? 84 : 92);
    const int right = std::max(gap * 2, width - gap);
    const int available = right - gap;
    int y = gap;
    int x = gap;
    auto place = [&](HWND control, int controlWidth) {
        if (control == nullptr) {
            return;
        }
        if (x > gap && x + controlWidth > right) {
            x = gap;
            y += rowHeight + gap;
        }
        placements.push_back(
            {control, RECT{x, y, x + controlWidth, y + rowHeight}});
        x += controlWidth + itemGap;
    };

    // 第一行：查找框尽量占满，三个复选框放得下就并排，否则整体换到下一行。
    const int checkTotal = checkWidth * 3 + itemGap * 2;
    const int minFindWidth = Scale(180);
    const bool checksInline = available >= minFindWidth + itemGap + checkTotal;
    const int findWidth = checksInline ? available - itemGap - checkTotal : available;
    if (const HWND find = ::GetDlgItem(hwnd_, kSearchPanelFind); find != nullptr) {
        placements.push_back({find, RECT{x, y, x + findWidth, y + rowHeight}});
        x += findWidth + itemGap;
    }
    if (checksInline) {
        for (const int id : {kSearchPanelCase, kSearchPanelWord, kSearchPanelRegex}) {
            place(::GetDlgItem(hwnd_, id), checkWidth);
        }
    } else {
        x = gap;
        y += rowHeight + gap;
        for (const int id : {kSearchPanelCase, kSearchPanelWord, kSearchPanelRegex}) {
            place(::GetDlgItem(hwnd_, id), checkWidth);
        }
    }

    // 第二行：五个操作按钮按空间换行；状态文字优先贴右，放不下再单独占一行。
    y += rowHeight + gap;
    x = gap;
    const int actionWidths[] = {buttonWidth, replaceButtonWidth, replaceButtonWidth,
                                smallButton, smallButton};
    const int actionIds[] = {kSearchPanelStart, kSearchPanelReplacePreview,
                             kSearchPanelApply, kSearchPanelStop, kSearchPanelClose};
    for (int index = 0; index < 5; ++index) {
        place(::GetDlgItem(hwnd_, actionIds[index]), actionWidths[index]);
    }
    const int statusMinWidth = Scale(compact ? 130 : 160);
    const int statusWidth = std::min(Scale(260), std::max(statusMinWidth, width / 3));
    if (status_ != nullptr) {
        if (x > gap && right - x >= statusMinWidth) {
            const int statusLeft = std::max(x, right - statusWidth);
            placements.push_back(
                {status_, RECT{statusLeft, y, right, y + rowHeight}});
        } else {
            x = gap;
            y += rowHeight + gap;
            placements.push_back({status_, RECT{x, y, right, y + rowHeight}});
        }
    }

    // 第三行起：目录 / 过滤器 / 替换框。宽度够就三列，窄了先让目录独占一行，
    // 再窄就把三个输入框逐行铺满。
    y += rowHeight + gap;
    x = gap;
    const int filterWidth = Scale(compact ? 150 : 220);
    const int replaceWidth = Scale(compact ? 130 : 180);
    const int minDirWidth = Scale(compact ? 150 : 180);
    const bool fieldsInline = available >=
        minDirWidth + itemGap * 2 + filterWidth + replaceWidth;
    const HWND dir = ::GetDlgItem(hwnd_, kSearchPanelDir);
    const HWND filter = ::GetDlgItem(hwnd_, kSearchPanelFilter);
    const HWND replace = ::GetDlgItem(hwnd_, kSearchPanelReplaceEdit);
    if (fieldsInline) {
        const int dirWidth = available - itemGap * 2 - filterWidth - replaceWidth;
        placements.push_back({dir, RECT{x, y, x + dirWidth, y + rowHeight}});
        x += dirWidth + itemGap;
        placements.push_back({filter, RECT{x, y, x + filterWidth, y + rowHeight}});
        x += filterWidth + itemGap;
        placements.push_back({replace, RECT{x, y, x + replaceWidth, y + rowHeight}});
    } else if (available >= Scale(200)) {
        placements.push_back({dir, RECT{x, y, x + available, y + rowHeight}});
        y += rowHeight + gap;
        const int halfWidth = (available - itemGap) / 2;
        placements.push_back({filter, RECT{x, y, x + halfWidth, y + rowHeight}});
        x += halfWidth + itemGap;
        placements.push_back(
            {replace, RECT{x, y, right, y + rowHeight}});
    } else {
        for (const HWND control : {dir, filter, replace}) {
            placements.push_back({control, RECT{x, y, right, y + rowHeight}});
            y += rowHeight + gap;
        }
    }
    return placements;
}

void SearchPanel::Layout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    int width = client.right - client.left;
    if (width <= 0) {
        RECT parent{};
        ::GetClientRect(::GetParent(hwnd_), &parent);
        width = parent.right - parent.left;
    }
    int bottom = 0;
    for (const Placement& placement : BuildPlacements(width)) {
        if (placement.handle != nullptr) {
            ::MoveWindow(placement.handle, placement.rect.left, placement.rect.top,
                         placement.rect.right - placement.rect.left,
                         placement.rect.bottom - placement.rect.top, TRUE);
            bottom = std::max(bottom, static_cast<int>(placement.rect.bottom));
        }
    }
    const int gap = Scale(kGap);
    const int listTop = bottom + gap;
    const int listHeight = std::max(Scale(kRowHeight), static_cast<int>(client.bottom) - listTop - gap);
    if (list_ != nullptr) {
        ::MoveWindow(list_, gap, listTop, std::max(10, width - gap * 2), listHeight, TRUE);
        const int total = std::max(100, width - gap * 2 - Scale(24));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 0, total * 35 / 100);
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 1, Scale(56));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 2, total * 60 / 100);
    }
    ScheduleScrollCorner();
}

void SearchPanel::Show() {
    Layout();
    ::ShowWindow(hwnd_, SW_SHOW);
}

void SearchPanel::Hide() {
    ::ShowWindow(hwnd_, SW_HIDE);
}

bool SearchPanel::Visible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) != 0;
}

std::wstring SearchPanel::TextOf(int controlId) const {
    const HWND control = ::GetDlgItem(hwnd_, controlId);
    if (control == nullptr) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void SearchPanel::SetTextOf(int controlId, const std::wstring& text) {
    if (HWND control = ::GetDlgItem(hwnd_, controlId); control != nullptr) {
        ::SetWindowTextW(control, text.c_str());
    }
}

search::SearchConfig SearchPanel::Config() const {
    search::SearchConfig config;
    config.pattern = TextOf(kSearchPanelFind);
    config.replaceTo = TextOf(kSearchPanelReplaceEdit);
    config.matchCase = ::IsDlgButtonChecked(hwnd_, kSearchPanelCase) == BST_CHECKED;
    config.wholeWordOnly = ::IsDlgButtonChecked(hwnd_, kSearchPanelWord) == BST_CHECKED;
    config.mode = (::IsDlgButtonChecked(hwnd_, kSearchPanelRegex) == BST_CHECKED)
                      ? search::SearchMode::Regex
                      : search::SearchMode::Literal;
    config.filter = TextOf(kSearchPanelFilter);
    if (config.filter.empty()) {
        config.filter = L"*.*";
    }
    const std::wstring directory = TextOf(kSearchPanelDir);
    if (!directory.empty()) {
        config.directories.push_back(directory);
    }
    return config;
}

std::wstring SearchPanel::Pattern() const {
    return TextOf(kSearchPanelFind);
}

void SearchPanel::SetPattern(const std::wstring& text) {
    SetTextOf(kSearchPanelFind, text);
}

void SearchPanel::SetDirectory(const std::wstring& directory) {
    if (TextOf(kSearchPanelDir).empty()) {
        SetTextOf(kSearchPanelDir, directory);
    }
}

void SearchPanel::SetFilter(const std::wstring& filter) {
    SetTextOf(kSearchPanelFilter, filter);
}

void SearchPanel::SetDefaultFilter() {
    if (TextOf(kSearchPanelFilter).empty()) {
        SetTextOf(kSearchPanelFilter, L"*.*");
    }
}

void SearchPanel::ClearResults() {
    rows_.clear();
    truncated_ = false;
    plans_.clear();
    replacePreview_ = false;
    if (list_ != nullptr) {
        ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
        // 回到"查找结果"的列定义
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT;
        column.pszText = const_cast<LPWSTR>(L"文件");
        ::SendMessageW(list_, LVM_SETCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        column.pszText = const_cast<LPWSTR>(L"行");
        ::SendMessageW(list_, LVM_SETCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
        column.pszText = const_cast<LPWSTR>(L"内容");
        ::SendMessageW(list_, LVM_SETCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
        ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, 0);
    }
    SetApplyEnabled(false);
    ScheduleScrollCorner();
}

void SearchPanel::BeginReplacePreview() {
    ClearResults();
    replacePreview_ = true;
    if (list_ == nullptr) {
        return;
    }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT;
    column.pszText = const_cast<LPWSTR>(L"文件");
    ::SendMessageW(list_, LVM_SETCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    column.pszText = const_cast<LPWSTR>(L"处数");
    ::SendMessageW(list_, LVM_SETCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
    column.pszText = const_cast<LPWSTR>(L"替换预览（改前 → 改后）");
    ::SendMessageW(list_, LVM_SETCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
    ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
}

void SearchPanel::AppendReplacePlan(const search::FileReplacePlan& plan) {
    if (list_ == nullptr || !replacePreview_) {
        return;
    }
    if (plans_.size() >= kMaxRows) {
        if (!truncated_) {
            truncated_ = true;
            SetStatus(L"文件过多，预览已截断");
        }
        return;
    }

    const std::size_t planIndex = plans_.size();
    plans_.push_back(plan);

    // 预览列显示第一处改动的前后对照；出错的文件把原因写在这里
    std::wstring detail;
    if (!plan.error.empty()) {
        detail = L"⚠ " + plan.error;
    } else if (!plan.samples.empty()) {
        detail = search::ConvertFromDocBytes(plan.samples[0].before, true, 936);
        detail += L"  →  ";
        detail += search::ConvertFromDocBytes(plan.samples[0].after, true, 936);
    }
    if (detail.size() > 200) {
        detail.resize(200);
        detail += L"…";
    }

    const std::wstring countText =
        plan.error.empty() ? std::to_wstring(plan.replacements) : std::wstring(L"—");
    const std::wstring displayName = plan.path;

    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(planIndex);
    item.iSubItem = 0;
    item.pszText = const_cast<LPWSTR>(displayName.c_str());
    item.lParam = static_cast<LPARAM>(planIndex);
    const int inserted = static_cast<int>(
        ::SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
    if (inserted < 0) {
        plans_.pop_back();
        return;
    }
    SetListSubItem(list_, inserted, 1, countText);
    SetListSubItem(list_, inserted, 2, detail);

    // 默认勾选；出错的文件取消勾选并置灰
    const UINT state = plan.selected ? 2u : 1u;
    LVITEMW stateItem{};
    stateItem.mask = LVIF_STATE;
    stateItem.stateMask = LVIS_STATEIMAGEMASK;
    stateItem.state = INDEXTOSTATEIMAGEMASK(state);
    ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(inserted),
                   reinterpret_cast<LPARAM>(&stateItem));

    rows_.push_back(RowRef{plan.path, 0, planIndex});
    ScheduleScrollCorner();
}

void SearchPanel::EndReplacePreview() {
    if (list_ != nullptr) {
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 0, Scale(320));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 1, Scale(60));
        ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, 2, Scale(520));
    }
    SetApplyEnabled(!plans_.empty());
    ScheduleScrollCorner();
}

std::vector<search::FileReplacePlan> SearchPanel::CheckedPlans() const {
    std::vector<search::FileReplacePlan> selected;
    if (list_ == nullptr || !replacePreview_) {
        return selected;
    }
    const int count = static_cast<int>(::SendMessageW(list_, LVM_GETITEMCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
        const LRESULT state = ::SendMessageW(list_, LVM_GETITEMSTATE,
                                             static_cast<WPARAM>(index), LVIS_STATEIMAGEMASK);
        const bool checked = ((state >> 12) & 0xF) == 2;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = index;
        if (::SendMessageW(list_, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) == 0) {
            continue;
        }
        const std::size_t planIndex = static_cast<std::size_t>(item.lParam);
        if (planIndex >= plans_.size()) {
            continue;
        }
        search::FileReplacePlan plan = plans_[planIndex];
        plan.selected = checked && plan.error.empty();
        selected.push_back(std::move(plan));
    }
    return selected;
}

std::wstring SearchPanel::ReplacementText() const {
    return TextOf(kSearchPanelReplaceEdit);
}

void SearchPanel::SetApplyEnabled(bool enabled) {
    if (HWND control = ::GetDlgItem(hwnd_, kSearchPanelApply); control != nullptr) {
        ::EnableWindow(control, enabled ? TRUE : FALSE);
    }
}

void SearchPanel::AppendFileResult(const search::FileSearchResult& result) {
    if (list_ == nullptr || result.hits.empty()) {
        return;
    }
    const std::wstring displayName = result.path;

    for (const search::LineHit& hit : result.hits) {
        if (rows_.size() >= kMaxRows) {
            if (!truncated_) {
                truncated_ = true;
                SetStatus(L"结果过多，已停止追加");
            }
            return;
        }

        // 行文本转成宽字符显示；过长时截断，免得列表被单行撑爆
        std::wstring text = search::ConvertFromDocBytes(hit.text, true, 936);
        if (text.size() > 400) {
            text.resize(400);
            text += L"…";
        }
        for (wchar_t& ch : text) {
            if (ch == L'\t') {
                ch = L' ';
            }
        }

        const int index = static_cast<int>(rows_.size());
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = index;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(displayName.c_str());
        item.lParam = index;
        const int inserted = static_cast<int>(::SendMessageW(list_, LVM_INSERTITEMW, 0,
                                                             reinterpret_cast<LPARAM>(&item)));
        if (inserted < 0) {
            return;
        }
        const std::wstring lineText = std::to_wstring(hit.line);
        SetListSubItem(list_, inserted, 1, lineText);
        SetListSubItem(list_, inserted, 2, text);
        rows_.push_back(RowRef{result.path, hit.line});
    }
    ScheduleScrollCorner();
}

void SearchPanel::SetStatus(const std::wstring& text) {
    if (status_ != nullptr) {
        ::SetWindowTextW(status_, text.c_str());
    }
}

void SearchPanel::SetRunning(bool running) {
    if (HWND control = ::GetDlgItem(hwnd_, kSearchPanelStart); control != nullptr) {
        ::EnableWindow(control, running ? FALSE : TRUE);
    }
    if (HWND control = ::GetDlgItem(hwnd_, kSearchPanelStop); control != nullptr) {
        ::EnableWindow(control, running ? TRUE : FALSE);
    }
    for (const int id : {kSearchPanelReplacePreview, kSearchPanelApply}) {
        if (HWND control = ::GetDlgItem(hwnd_, id); control != nullptr) {
            ::EnableWindow(control, running ? FALSE : TRUE);
        }
    }
    if (!running) {
        SetApplyEnabled(replacePreview_ && !plans_.empty());
    }
}

bool SearchPanel::SelectedTarget(std::wstring& path, int& line) const {
    if (list_ == nullptr) {
        return false;
    }
    const int selected = static_cast<int>(::SendMessageW(list_, LVM_GETNEXTITEM,
                                                         static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (selected < 0) {
        return false;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (::SendMessageW(list_, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) == 0) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(item.lParam);
    if (index >= rows_.size()) {
        return false;
    }
    path = rows_[index].path;
    line = rows_[index].line;
    return true;
}

void SearchPanel::OnListDoubleClick() {
    // 交给父窗口去打开文件并跳转
    ::SendMessageW(::GetParent(hwnd_), WM_COMMAND,
                   MAKEWPARAM(kSearchPanelList, 0), 0);
}

LRESULT CALLBACK SearchPanel::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    SearchPanel* self = reinterpret_cast<SearchPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SearchPanel*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT SearchPanel::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd_, &paint);
            PaintBackground(dc);
            ::EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintBackground(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_SIZE:
            Layout();
            // 系统列表控件在尺寸变化后偶尔会退回浅色底；重新套一次主题，
            // 尤其是被压到只剩一两行时也能保持深色。
            if (dark_) {
                ApplyThemeToControls();
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Theme().text);
            if (reinterpret_cast<HWND>(lParam) == scrollCorner_) {
                return reinterpret_cast<LRESULT>(
                    field_ != nullptr ? field_ : ::GetSysColorBrush(COLOR_WINDOW));
            }
            return reinterpret_cast<LRESULT>(
                background_ != nullptr ? background_ : ::GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Theme().text);
            ::SetBkColor(dc, Theme().tabActive);
            return reinterpret_cast<LRESULT>(
                field_ != nullptr ? field_ : ::GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom == kSearchPanelList && header->code == NM_DBLCLK) {
                OnListDoubleClick();
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            ::SendMessageW(::GetParent(hwnd_), WM_COMMAND, wParam, lParam);
            return 0;
        case WM_THEMECHANGED:
            ApplyThemeToControls();
            ::InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case kMsgUpdateScrollCorner:
            UpdateScrollCorner();
            return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::shell
