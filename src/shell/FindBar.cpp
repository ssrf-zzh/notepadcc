#include "shell/FindBar.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>

#include "infra/Log.h"

namespace npcc::shell {
namespace {

constexpr int kRowHeight = 26;
constexpr int kGap = 6;

// 让查找/替换输入框把 Enter 与 Esc 转交给查找栏：
// 单行 EDIT 默认会吞掉这两个键。
LRESULT CALLBACK EditSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                              UINT_PTR /*id*/, DWORD_PTR ref) {
    if (message == WM_KEYDOWN) {
        const HWND parent = reinterpret_cast<HWND>(ref);
        if (wParam == VK_ESCAPE) {
            ::PostMessageW(parent, WM_COMMAND, kCloseButton, 0);
            return 0;
        }
        if (wParam == VK_RETURN) {
            const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            ::PostMessageW(parent, WM_COMMAND, shift ? kFindPrevButton : kFindNextButton, 0);
            return 0;
        }
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK StatusSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    if (message == WM_ERASEBKGND) {
        return 1;   // 由父窗口擦除，避免闪烁
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

}  // namespace

bool FindBar::Create(HWND parent) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &FindBar::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // 背景由 WM_PAINT 按主题填，类刷子会把深色 / 护眼底色盖回系统灰
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"notepadccFindBar";
    ::RegisterClassExW(&wc);   // 重复注册无害

    hwnd_ = ::CreateWindowExW(0, L"notepadccFindBar", L"",
                              WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr,
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

    findEdit_ = MakeControl(L"EDIT",
                            L"",
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                            kFindEdit);
    replaceEdit_ = MakeControl(L"EDIT",
                               L"",
                               WS_CHILD | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                               kReplaceEdit);
    modeCombo_ = MakeControl(WC_COMBOBOXW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                 WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                             kModeCombo);
    statusText_ = MakeControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, -1);

    if (modeCombo_ != nullptr) {
        ::SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"普通"));
        ::SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"扩展"));
        ::SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"正则"));
        ::SendMessageW(modeCombo_, CB_SETCURSEL, 0, 0);
    }

    MakeControl(L"BUTTON", L"区分大小写", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                kCaseCheck);
    MakeControl(L"BUTTON", L"全字匹配", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                kWordCheck);
    MakeControl(L"BUTTON", L".匹配换行", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                kDotAllCheck);
    MakeControl(L"BUTTON", L"上一个", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                kFindPrevButton);
    MakeControl(L"BUTTON", L"下一个", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                kFindNextButton);
    MakeControl(L"BUTTON", L"计数", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                kCountButton);
    MakeControl(L"BUTTON", L"标记", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                kMarkButton);
    MakeControl(L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                kCloseButton);
    MakeControl(L"BUTTON", L"替换", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, kReplaceButton);
    MakeControl(L"BUTTON", L"全部替换", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                kReplaceAllButton);

    ApplyFont();
    Layout();
    SetTheme(dark_, eyeCare_);
    return true;
}

void FindBar::Destroy() {
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
}

HWND FindBar::MakeControl(const wchar_t* className, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, className, text, style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void FindBar::ApplyFont() {
    // 与菜单栏 / 工具栏 / 状态栏同一套外壳字体；输入框里的等宽字体由编辑器自己负责
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
    if (modeCombo_ != nullptr) {
        ::SendMessageW(modeCombo_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Scale(24));
        ::SendMessageW(modeCombo_, CB_SETITEMHEIGHT, 0, Scale(24));
        ::SetWindowSubclass(modeCombo_, &FindBar::ComboSubclass, 4500,
                            reinterpret_cast<DWORD_PTR>(this));
    }
    if (findEdit_ != nullptr) {
        ::SetWindowSubclass(findEdit_, &EditSubclass, 1, reinterpret_cast<DWORD_PTR>(hwnd_));
    }
    if (replaceEdit_ != nullptr) {
        ::SetWindowSubclass(replaceEdit_, &EditSubclass, 2, reinterpret_cast<DWORD_PTR>(hwnd_));
    }
    if (statusText_ != nullptr) {
        ::SetWindowSubclass(statusText_, &StatusSubclass, 3, 0);
    }
}

ChromeColours FindBar::Theme() const {
    return ChromeColoursFor(dark_, eyeCare_);
}

void FindBar::SetTheme(bool dark, bool eyeCare) {
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

void FindBar::UpdateThemeResources() {
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

void FindBar::ApplyThemeToControls() {
    if (hwnd_ == nullptr) {
        return;
    }
    ::EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM param) -> BOOL {
            const auto* self = reinterpret_cast<const FindBar*>(param);
            wchar_t className[32]{};
            ::GetClassNameW(child, className, 32);
            if (::_wcsicmp(className, L"Edit") == 0) {
                ApplyControlTheme(child, self->dark_, L"DarkMode_CFD");
            } else if (::_wcsicmp(className, L"ComboBox") == 0) {
                ApplyControlTheme(child, self->dark_, L"DarkMode_Explorer");
            } else if (::_wcsicmp(className, L"Button") == 0) {
                ::InvalidateRect(child, nullptr, TRUE);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
    if (modeCombo_ != nullptr) {
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        if (::GetComboBoxInfo(modeCombo_, &info) && info.hwndList != nullptr) {
            ApplyControlTheme(info.hwndList, dark_, L"DarkMode_Explorer");
            ::InvalidateRect(info.hwndList, nullptr, TRUE);
        }
    }
}

void FindBar::InstallButtonSubclasses() {
    if (hwnd_ == nullptr) {
        return;
    }
    ::EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t className[32]{};
            ::GetClassNameW(child, className, 32);
            if (::_wcsicmp(className, L"Button") == 0) {
                ::SetWindowSubclass(child, &FindBar::ButtonSubclass,
                                    static_cast<UINT_PTR>(4000 + ::GetDlgCtrlID(child)), param);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
}

void FindBar::PaintBackground(HDC dc) const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const HBRUSH brush = background_ != nullptr ? background_ : ::GetSysColorBrush(COLOR_WINDOW);
    ::FillRect(dc, &client, brush);
}

void FindBar::PaintControl(HWND control, HDC dc) const {
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
    const bool primary = controlId == kFindNextButton || controlId == kReplaceAllButton;
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

void FindBar::PaintCombo(HDC dc) const {
    if (modeCombo_ == nullptr || dc == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(modeCombo_, &client);
    const ChromeColours colours = Theme();
    const bool enabled = ::IsWindowEnabled(modeCombo_) != 0;
    const bool focused = ::GetFocus() == modeCombo_;
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const bool hovered = ::WindowFromPoint(cursor) == modeCombo_;

    const HBRUSH outer = background_ != nullptr ? background_
                                                : ::GetSysColorBrush(COLOR_BTNFACE);
    ::FillRect(dc, &client, outer);
    RECT body = client;
    ::InflateRect(&body, -Scale(1), -Scale(1));
    const int radius = Scale(6);
    const COLORREF border = focused ? colours.accent
                                    : (hovered ? colours.textMuted : colours.line);
    const HBRUSH fill = ::CreateSolidBrush(enabled ? colours.tabActive : colours.strip);
    const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, Scale(1)),
                                 enabled ? border : colours.line);
    const HGDIOBJ oldBrush = ::SelectObject(dc, fill);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::RoundRect(dc, body.left, body.top, body.right, body.bottom, radius, radius);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(pen);
    ::DeleteObject(fill);

    wchar_t text[128]{};
    const LRESULT selected = ::SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0);
    if (selected != CB_ERR) {
        ::SendMessageW(modeCombo_, CB_GETLBTEXT, static_cast<WPARAM>(selected),
                       reinterpret_cast<LPARAM>(text));
    }
    const HGDIOBJ oldFont = font_ != nullptr ? ::SelectObject(dc, font_) : nullptr;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, enabled ? colours.text : colours.iconDisabled);
    RECT textRect = body;
    textRect.left += Scale(10);
    textRect.right -= Scale(28);
    ::DrawTextW(dc, text, -1, &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    const int arrowX = body.right - Scale(15);
    const int arrowY = (body.top + body.bottom) / 2;
    const HPEN arrowPen = ::CreatePen(PS_SOLID, std::max(1, Scale(1)), colours.textMuted);
    const HGDIOBJ previousPen = ::SelectObject(dc, arrowPen);
    ::MoveToEx(dc, arrowX - Scale(4), arrowY - Scale(2), nullptr);
    ::LineTo(dc, arrowX, arrowY + Scale(2));
    ::LineTo(dc, arrowX + Scale(4), arrowY - Scale(2));
    ::SelectObject(dc, previousPen);
    ::DeleteObject(arrowPen);
    if (oldFont != nullptr) {
        ::SelectObject(dc, oldFont);
    }
}

LRESULT CALLBACK FindBar::ButtonSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<FindBar*>(ref);
    return self != nullptr ? self->ButtonSubclassProc(hwnd, message, wParam, lParam)
                           : ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT FindBar::ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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

LRESULT CALLBACK FindBar::ComboSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<FindBar*>(ref);
    return self != nullptr ? self->ComboSubclassProc(hwnd, message, wParam, lParam)
                           : ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT FindBar::ComboSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd, &paint);
            PaintCombo(dc);
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintCombo(reinterpret_cast<HDC>(wParam));
            return 0;
        case CB_SETCURSEL: {
            const LRESULT result = ::DefSubclassProc(hwnd, message, wParam, lParam);
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return result;
        }
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
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

int FindBar::Scale(int value) const {
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

int FindBar::Height() const {
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
    return bottom + Scale(kGap);
}

std::vector<FindBar::Placement> FindBar::BuildPlacements(int width) const {
    std::vector<Placement> placements;
    if (hwnd_ == nullptr || width <= 0) {
        return placements;
    }
    const int gap = Scale(kGap);
    const int itemGap = Scale(6);
    const int rowHeight = Scale(kRowHeight);
    const int statusWidth = std::min(Scale(220), std::max(Scale(120), width / 4));
    const int comboWidth = Scale(76);
    const int right = std::max(gap * 2, width - gap);
    const int available = right - gap;
    const int minFindWidth = Scale(120);
    const bool statusInline =
        available >= minFindWidth + itemGap + comboWidth + itemGap + Scale(150);
    const int findWidth = std::max(
        minFindWidth, available - comboWidth - itemGap - (statusInline ? statusWidth + itemGap : 0));

    int y = gap;
    int x = gap;
    placements.push_back({findEdit_, RECT{x, y, x + findWidth, y + rowHeight}});
    x += findWidth + itemGap;
    placements.push_back({modeCombo_, RECT{x, y, x + comboWidth, y + rowHeight}});
    if (statusInline) {
        placements.push_back(
            {statusText_, RECT{right - statusWidth, y, right, y + rowHeight}});
    } else {
        y += rowHeight + gap;
        placements.push_back({statusText_, RECT{gap, y, right, y + rowHeight}});
    }

    y += rowHeight + gap;
    x = gap;
    auto place = [&](HWND control, int controlWidth) {
        if (control == nullptr) {
            return;
        }
        if (x > gap && x + controlWidth > width - gap) {
            x = gap;
            y += rowHeight + gap;
        }
        placements.push_back(
            {control, RECT{x, y, x + controlWidth, y + rowHeight}});
        x += controlWidth + itemGap;
    };

    // 「区分大小写」5 个汉字 + 复选框，84 像素会在闭合态画出省略号。
    const int checkWidth = Scale(104);
    const int buttonWidth = Scale(60);
    for (const int id : {kCaseCheck, kWordCheck, kDotAllCheck}) {
        place(::GetDlgItem(hwnd_, id), checkWidth);
    }
    for (const int id : {kFindPrevButton, kFindNextButton, kCountButton, kMarkButton,
                         kCloseButton}) {
        place(::GetDlgItem(hwnd_, id), buttonWidth);
    }

    if (replaceMode_) {
        y += rowHeight + gap;
        x = gap;
        const int replaceButton = Scale(60);
        const int replaceAllButton = Scale(90);
        const bool replaceInline =
            available >= Scale(120) + itemGap * 2 + replaceButton + replaceAllButton;
        if (replaceInline) {
            const int replaceWidth =
                available - itemGap * 2 - replaceButton - replaceAllButton;
            placements.push_back(
                {replaceEdit_, RECT{x, y, x + replaceWidth, y + rowHeight}});
            x += replaceWidth + itemGap;
            placements.push_back(
                {::GetDlgItem(hwnd_, kReplaceButton),
                 RECT{x, y, x + replaceButton, y + rowHeight}});
            x += replaceButton + itemGap;
            placements.push_back(
                {::GetDlgItem(hwnd_, kReplaceAllButton),
                 RECT{x, y, x + replaceAllButton, y + rowHeight}});
        } else {
            placements.push_back({replaceEdit_, RECT{x, y, right, y + rowHeight}});
            y += rowHeight + gap;
            x = gap;
            placements.push_back(
                {::GetDlgItem(hwnd_, kReplaceButton),
                 RECT{x, y, x + replaceButton, y + rowHeight}});
            x += replaceButton + itemGap;
            placements.push_back(
                {::GetDlgItem(hwnd_, kReplaceAllButton),
                 RECT{x, y, x + replaceAllButton, y + rowHeight}});
        }
    }
    return placements;
}

void FindBar::Layout() {
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
    for (const Placement& placement : BuildPlacements(width)) {
        if (placement.handle != nullptr) {
            ::MoveWindow(placement.handle, placement.rect.left, placement.rect.top,
                         placement.rect.right - placement.rect.left,
                         placement.handle == modeCombo_
                             ? Scale(kRowHeight) * 6
                             : placement.rect.bottom - placement.rect.top,
                         TRUE);
        }
    }
}

void FindBar::Show(bool replaceMode) {
    replaceMode_ = replaceMode;
    if (replaceEdit_ != nullptr) {
        ::ShowWindow(replaceEdit_, replaceMode ? SW_SHOW : SW_HIDE);
    }
    const bool showReplaceButtons = replaceMode;
    for (const int id : {kReplaceButton, kReplaceAllButton}) {
        if (HWND control = ::GetDlgItem(hwnd_, id); control != nullptr) {
            ::ShowWindow(control, showReplaceButtons ? SW_SHOW : SW_HIDE);
        }
    }
    Layout();
    ::ShowWindow(hwnd_, SW_SHOW);
    FocusFindField();
}

void FindBar::Hide() {
    ::ShowWindow(hwnd_, SW_HIDE);
}

bool FindBar::Visible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) != 0;
}

void FindBar::FocusFindField() {
    if (findEdit_ != nullptr) {
        ::SetFocus(findEdit_);
        ::SendMessageW(findEdit_, EM_SETSEL, 0, -1);
    }
}

std::wstring FindBar::TextOf(int controlId) const {
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

int FindBar::ModeIndex() const {
    if (modeCombo_ == nullptr) {
        return 0;
    }
    const LRESULT index = ::SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0);
    return index == CB_ERR ? 0 : static_cast<int>(index);
}

search::SearchConfig FindBar::Config(bool utfMode, std::uint32_t codePage) const {
    search::SearchConfig config;
    config.pattern = TextOf(kFindEdit);
    config.replaceTo = TextOf(kReplaceEdit);
    config.mode = static_cast<search::SearchMode>(ModeIndex());
    config.matchCase = ::IsDlgButtonChecked(hwnd_, kCaseCheck) == BST_CHECKED;
    config.wholeWordOnly = ::IsDlgButtonChecked(hwnd_, kWordCheck) == BST_CHECKED;
    config.dotMatchesNewline = ::IsDlgButtonChecked(hwnd_, kDotAllCheck) == BST_CHECKED;
    config.utfMode = utfMode;
    config.codePage = codePage;
    return config;
}

void FindBar::SetStatus(const std::wstring& text) {
    if (statusText_ != nullptr) {
        ::SetWindowTextW(statusText_, text.c_str());
    }
}

std::wstring FindBar::Pattern() const {
    return TextOf(kFindEdit);
}

void FindBar::SetPattern(const std::wstring& text) {
    if (findEdit_ != nullptr) {
        ::SetWindowTextW(findEdit_, text.c_str());
    }
}

LRESULT CALLBACK FindBar::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    FindBar* self = reinterpret_cast<FindBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<FindBar*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT FindBar::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
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
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Theme().text);
            return reinterpret_cast<LRESULT>(
                background_ != nullptr ? background_ : ::GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, Theme().text);
            ::SetBkColor(dc, Theme().tabActive);
            return reinterpret_cast<LRESULT>(
                field_ != nullptr ? field_ : ::GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw == nullptr || draw->CtlID != kModeCombo || modeCombo_ == nullptr) {
                break;
            }
            wchar_t text[128]{};
            if (draw->itemID == static_cast<UINT>(-1)) {
                const int selected = ModeIndex();
                ::SendMessageW(modeCombo_, CB_GETLBTEXT, static_cast<WPARAM>(selected),
                               reinterpret_cast<LPARAM>(text));
            } else {
                ::SendMessageW(modeCombo_, CB_GETLBTEXT, draw->itemID,
                               reinterpret_cast<LPARAM>(text));
            }
            const ChromeColours colours = Theme();
            const bool selected = (draw->itemState & ODS_SELECTED) != 0;
            const HBRUSH fill = ::CreateSolidBrush(selected ? colours.accentSoft
                                                            : colours.tabActive);
            ::FillRect(draw->hDC, &draw->rcItem, fill);
            ::DeleteObject(fill);
            const HGDIOBJ oldFont = font_ != nullptr ? ::SelectObject(draw->hDC, font_) : nullptr;
            ::SetBkMode(draw->hDC, TRANSPARENT);
            ::SetTextColor(draw->hDC, colours.text);
            RECT textRect = draw->rcItem;
            textRect.left += Scale(10);
            textRect.right -= Scale(4);
            ::DrawTextW(draw->hDC, text, -1, &textRect,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
            if (oldFont != nullptr) {
                ::SelectObject(draw->hDC, oldFont);
            }
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kModeCombo && HIWORD(wParam) == CBN_SELCHANGE &&
                modeCombo_ != nullptr) {
                ::InvalidateRect(modeCombo_, nullptr, FALSE);
            }
            // 控件事件上抛给主窗口统一处理
            ::SendMessageW(::GetParent(hwnd_), WM_COMMAND, wParam, lParam);
            return 0;
        case WM_THEMECHANGED:
            ApplyThemeToControls();
            ::InvalidateRect(hwnd_, nullptr, TRUE);
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
