#include "shell/ToolBar.h"

#include "shell/ChromeStyle.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

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

struct Colors {
    COLORREF bg = 0;
    COLORREF hot = 0;
    COLORREF pressed = 0;
    COLORREF checked = 0;
    COLORREF accent = 0;
    COLORREF fg = 0;
    COLORREF disabled = 0;
    COLORREF separator = 0;
};

// 工具栏配色跟 chrome 一套（M3 第十四块）：底色与标签栏同色，悬停 / 按下 / 勾选
// 都是圆角浅底，勾选的图标本身用强调色——不再画硬边框。护眼跟着深浅成四套。
Colors PaletteOf(bool dark, bool eyeCare) {
    const ChromeColours chrome = ChromeColoursFor(dark, eyeCare);
    Colors colors;
    colors.bg = chrome.strip;
    colors.hot = chrome.buttonHot;
    colors.pressed = chrome.buttonPressed;
    colors.checked = chrome.accentSoft;
    colors.accent = chrome.accent;
    colors.fg = chrome.text;
    colors.disabled = chrome.iconDisabled;
    colors.separator = chrome.line;
    return colors;
}

bool KeyHas(const std::wstring& key, const wchar_t* token) {
    return key.find(token) != std::wstring::npos;
}

// 图标：按目标尺寸现画的线条画（设计文档 6.4 说"单色矢量位图光栅化"，这里再省一步，
// 直接用 GDI 画——任何 DPI 都不糊，也不用维护六套位图资源）。
// 认不出的键画一个圆角方块：宁可给个中性记号，也别让按钮看起来是空的。
void PaintGlyph(HDC dc, const RECT& box, const std::wstring& key, COLORREF colour) {
    const int size = box.right - box.left;
    if (size <= 4) {
        return;
    }
    const int unit = std::max(2, size / 8);
    const int stroke = std::max(1, size / 12);
    const int left = box.left;
    const int top = box.top;
    const int right = box.right - 1;
    const int bottom = box.bottom - 1;

    HPEN pen = ::CreatePen(PS_SOLID, stroke, colour);
    const HGDIOBJ oldPen = ::SelectObject(dc, pen);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    HBRUSH fill = ::CreateSolidBrush(colour);

    if (KeyHas(key, L".new")) {
        ::MoveToEx(dc, left + unit, bottom - unit, nullptr);
        ::LineTo(dc, left + unit, top);
        ::LineTo(dc, right - unit * 2, top);
        ::LineTo(dc, right - unit, top + unit * 2);
        ::LineTo(dc, right - unit, bottom - unit);
        ::LineTo(dc, left + unit, bottom - unit);
        ::MoveToEx(dc, right - unit * 2, top, nullptr);
        ::LineTo(dc, right - unit * 2, top + unit * 2);
        ::LineTo(dc, right - unit, top + unit * 2);
    } else if (KeyHas(key, L".open") || KeyHas(key, L"workspace")) {
        ::MoveToEx(dc, left, bottom - unit, nullptr);
        ::LineTo(dc, left, top + unit);
        ::LineTo(dc, left + unit * 3, top + unit);
        ::LineTo(dc, left + unit * 3, top);
        ::LineTo(dc, left + unit * 5, top);
        ::LineTo(dc, left + unit * 5, top + unit);
        ::LineTo(dc, right, top + unit);
        ::LineTo(dc, right, bottom - unit);
        ::LineTo(dc, left, bottom - unit);
    } else if (KeyHas(key, L".save")) {
        ::Rectangle(dc, left, top, right, bottom);
        ::Rectangle(dc, left + unit * 2, top, right - unit * 2, top + unit * 2);
        ::Rectangle(dc, left + unit, bottom - unit * 3, right - unit, bottom);
    } else if (KeyHas(key, L".undo") || KeyHas(key, L".redo")) {
        const bool forward = KeyHas(key, L".redo");
        const int head = forward ? right - unit : left + unit;
        const int tail = forward ? left + unit : right - unit;
        const int step = forward ? -unit * 2 : unit * 2;
        ::MoveToEx(dc, head, top + unit * 3, nullptr);
        ::LineTo(dc, head + step, top + unit);
        ::MoveToEx(dc, head, top + unit * 3, nullptr);
        ::LineTo(dc, head + step, top + unit * 5);
        ::MoveToEx(dc, head, top + unit * 3, nullptr);
        ::LineTo(dc, tail, top + unit * 3);
        ::LineTo(dc, tail, bottom - unit);
    } else if (KeyHas(key, L".find") || KeyHas(key, L"zoom")) {
        ::Ellipse(dc, left, top, left + unit * 5, top + unit * 5);
        ::MoveToEx(dc, left + unit * 4, top + unit * 4, nullptr);
        ::LineTo(dc, right, bottom);
        if (KeyHas(key, L"zoomOut")) {
            ::MoveToEx(dc, left + unit * 2, top + unit * 2, nullptr);
            ::LineTo(dc, left + unit * 3, top + unit * 2);
        } else if (KeyHas(key, L"zoom")) {
            ::MoveToEx(dc, left + unit * 2, top + unit * 2, nullptr);
            ::LineTo(dc, left + unit * 3, top + unit * 2);
            ::MoveToEx(dc, left + unit * 5 / 2, top + unit, nullptr);
            ::LineTo(dc, left + unit * 5 / 2, top + unit * 3);
        } else if (KeyHas(key, L"findNext")) {
            ::MoveToEx(dc, right - unit * 2, top, nullptr);
            ::LineTo(dc, right, top);
            ::LineTo(dc, right - unit, top + unit);
            ::LineTo(dc, right - unit * 2, top);
        }
    } else if (KeyHas(key, L"Bookmark") || KeyHas(key, L"bookmark")) {
        POINT ribbon[5] = {
            {left + unit, top},
            {right - unit * 2, top},
            {right - unit * 2, bottom},
            {left + unit * 5 / 2, bottom - unit},
            {left + unit, bottom},
        };
        ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::Polygon(dc, ribbon, 5);
        if (KeyHas(key, L"next") || KeyHas(key, L"Next")) {
            ::MoveToEx(dc, right - unit, top + unit * 2, nullptr);
            ::LineTo(dc, right, top + unit * 3);
            ::LineTo(dc, right - unit, top + unit * 4);
        }
    } else if (KeyHas(key, L"Comment") || KeyHas(key, L"comment")) {
        ::MoveToEx(dc, left + unit, bottom - unit, nullptr);
        ::LineTo(dc, left + unit * 2, top + unit);
        ::MoveToEx(dc, left + unit * 3, bottom - unit, nullptr);
        ::LineTo(dc, left + unit * 4, top + unit);
        ::MoveToEx(dc, left + unit * 5, top + unit * 2, nullptr);
        ::LineTo(dc, right, top + unit * 2);
        ::MoveToEx(dc, left + unit * 5, top + unit * 4, nullptr);
        ::LineTo(dc, right, top + unit * 4);
    } else if (KeyHas(key, L"oldAll") || KeyHas(key, L"oldToggle")) {
        ::MoveToEx(dc, left, top + unit * 3, nullptr);
        ::LineTo(dc, right, top + unit * 3);
        ::SelectObject(dc, fill);
        POINT up[3] = {{left + unit, top + unit * 2}, {left + unit * 3, top + unit * 2},
                       {left + unit * 2, top}};
        POINT down[3] = {{left + unit, bottom - unit * 2}, {left + unit * 3, bottom - unit * 2},
                         {left + unit * 2, bottom}};
        ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        if (KeyHas(key, L"nfold")) {   // Unfold：箭头朝外
            ::Polygon(dc, up, 3);
            ::Polygon(dc, down, 3);
        } else {                        // Fold：箭头朝中间那条线
            POINT up2[3] = {{left + unit, top}, {left + unit * 3, top}, {left + unit * 2, top + unit * 2}};
            POINT down2[3] = {{left + unit, bottom}, {left + unit * 3, bottom}, {left + unit * 2, bottom - unit * 2}};
            ::Polygon(dc, up2, 3);
            ::Polygon(dc, down2, 3);
        }
    } else if (KeyHas(key, L"DarkMode") || KeyHas(key, L"darkMode")) {
        // 月牙：外弧 + 内弧（内弧圆心右移），用两段折线拼出缺口
        const double cx = (left + right) / 2.0;
        const double cy = (top + bottom) / 2.0;
        const double radius = (size - 2) / 2.0;
        const double inner = radius * 0.78;
        POINT moon[42] = {};
        int count = 0;
        for (int i = 0; i <= 20; ++i) {
            const double angle = (-70.0 + 320.0 * i / 20.0) * 3.14159265358979 / 180.0;
            moon[count].x = static_cast<LONG>(cx + radius * std::cos(angle));
            moon[count].y = static_cast<LONG>(cy + radius * std::sin(angle));
            ++count;
        }
        for (int i = 20; i >= 0; --i) {
            const double angle = (-70.0 + 320.0 * i / 20.0) * 3.14159265358979 / 180.0;
            moon[count].x = static_cast<LONG>(cx + radius * 0.45 + inner * std::cos(angle));
            moon[count].y = static_cast<LONG>(cy + inner * std::sin(angle));
            ++count;
        }
        ::SelectObject(dc, fill);
        ::Polygon(dc, moon, count);
    } else if (KeyHas(key, L"split")) {
        ::Rectangle(dc, left, top, left + unit * 3, bottom);
        ::Rectangle(dc, left + unit * 4, top, right, bottom);
    } else if (KeyHas(key, L"hex")) {
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 2; ++column) {
                const int x = left + column * unit * 4;
                const int y = top + row * unit * 4;
                ::Rectangle(dc, x, y, x + unit * 3, y + unit * 3);
            }
        }
    } else if (KeyHas(key, L"print") || KeyHas(key, L"Export") || KeyHas(key, L"export")) {
        ::Rectangle(dc, left, top + unit * 3, right, bottom);
        ::MoveToEx(dc, left + unit * 2, top + unit * 3, nullptr);
        ::LineTo(dc, left + unit * 2, top);
        ::LineTo(dc, right - unit * 2, top);
        ::LineTo(dc, right - unit * 2, top + unit * 3);
    } else if (KeyHas(key, L"ummary") || KeyHas(key, L".info")) {
        ::Rectangle(dc, left, top, right, bottom);
        ::MoveToEx(dc, left + unit, top + unit * 2, nullptr);
        ::LineTo(dc, right - unit, top + unit * 2);
        ::MoveToEx(dc, left + unit, top + unit * 4, nullptr);
        ::LineTo(dc, right - unit, top + unit * 4);
        ::MoveToEx(dc, left + unit, top + unit * 6, nullptr);
        ::LineTo(dc, right - unit * 3, top + unit * 6);
    } else {
        ::Rectangle(dc, left, top, right, bottom);
        ::SelectObject(dc, fill);
        ::Ellipse(dc, left + unit * 2, top + unit * 2, left + unit * 5, top + unit * 5);
    }

    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(fill);
    ::DeleteObject(pen);
}

}  // namespace
bool ToolBar::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ToolBar::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // 背景由我们自己刷（深浅两套底色），不交给系统——工具栏是"跟着主题走"的那一层
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int ToolBar::Scale(int value) const {
    return ::MulDiv(value, DpiOf(hwnd_), 96);
}

int ToolBar::CellSize() const {
    return Scale(iconSize_) + Scale(3) * 2;
}

int ToolBar::SeparatorWidth() const {
    return Scale(7);
}

int ToolBar::Height() const {
    return visible_ ? CellSize() + 1 : 0;   // +1 是底下那条分隔线
}

int ToolBar::Width() const {
    if (!visible_) {
        return 0;
    }
    int width = Scale(4);
    for (std::size_t i = 0; i < items_.size(); ++i) {
        width += items_[i].id == 0 ? SeparatorWidth() : CellSize();
    }
    return width + Scale(4);
}

int ToolBar::ItemLeft(std::size_t index) const {
    int x = Scale(4);
    for (std::size_t i = 0; i < index && i < items_.size(); ++i) {
        x += items_[i].id == 0 ? SeparatorWidth() : CellSize();
    }
    return x;
}

bool ToolBar::Create(HWND parent, int controlId) {
    if (hwnd_ != nullptr) {
        return true;
    }
    hwnd_ = ::CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                              parent,
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

    tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
    if (tooltip_ != nullptr) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
        tool.hwnd = hwnd_;
        tool.uId = reinterpret_cast<UINT_PTR>(hwnd_);
        tool.lpszText = tipText_.data();   // 内容随后由 TTM_UPDATETIPTEXTW 换
        ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        if (font_ != nullptr) {
            ::SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }
    SetVisible(visible_);
    return true;
}

void ToolBar::SetVisible(bool visible) {
    visible_ = visible;
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible_ ? SW_SHOW : SW_HIDE);
    }
    if (!visible_) {
        HideTooltip();
        hot_ = -1;
        pressed_ = -1;
    }
}

void ToolBar::SetIconSize(int logicalPixels) {
    iconSize_ = std::max(12, std::min(48, logicalPixels));
    Refresh();
}

void ToolBar::SetItems(const core::ToolbarItems& keys) {
    items_.clear();
    items_.reserve(keys.size());
    for (const std::wstring& key : keys) {
        Item item;
        item.key = key;
        if (key != core::kToolbarSeparator) {
            if (!keyResolver_) {
                continue;
            }
            item.id = keyResolver_(key);
            if (item.id == 0) {
                continue;   // 解不出来（插件卸了、宏删了）：跳过，日志由外壳记
            }
        }
        items_.push_back(std::move(item));
    }
    hot_ = -1;
    pressed_ = -1;
    Refresh();
}

void ToolBar::SetDark(bool dark) {
    if (dark_ != dark) {
        dark_ = dark;
        Refresh();
    }
}

void ToolBar::SetEyeCare(bool eyeCare) {
    if (eyeCare_ != eyeCare) {
        eyeCare_ = eyeCare;
        Refresh();
    }
}

void ToolBar::Refresh() const {
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

int ToolBar::ButtonCount() const {
    int count = 0;
    for (const Item& item : items_) {
        if (item.id != 0) {
            ++count;
        }
    }
    return count;
}

int ToolBar::ButtonOrdinal(int itemIndex) const {
    if (itemIndex < 0 || static_cast<std::size_t>(itemIndex) >= items_.size() ||
        items_[static_cast<std::size_t>(itemIndex)].id == 0) {
        return -1;
    }
    int ordinal = 0;
    for (int i = 0; i < itemIndex; ++i) {
        if (items_[static_cast<std::size_t>(i)].id != 0) {
            ++ordinal;
        }
    }
    return ordinal;
}

int ToolBar::OrdinalItem(int ordinal) const {
    if (ordinal < 0) {
        return -1;
    }
    int seen = 0;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == 0) {
            continue;
        }
        if (seen == ordinal) {
            return static_cast<int>(i);
        }
        ++seen;
    }
    return -1;
}

bool ToolBar::EnabledAt(int itemIndex) const {
    if (itemIndex < 0 || static_cast<std::size_t>(itemIndex) >= items_.size()) {
        return false;
    }
    const Item& item = items_[static_cast<std::size_t>(itemIndex)];
    if (item.id == 0) {
        return false;
    }
    return enabledQuery_ ? enabledQuery_(item.id) : true;
}

bool ToolBar::CheckedAt(int itemIndex) const {
    if (itemIndex < 0 || static_cast<std::size_t>(itemIndex) >= items_.size()) {
        return false;
    }
    const Item& item = items_[static_cast<std::size_t>(itemIndex)];
    if (item.id == 0) {
        return false;
    }
    return checkedQuery_ ? checkedQuery_(item.id) : false;
}

std::wstring ToolBar::TextAt(int itemIndex) const {
    if (itemIndex < 0 || static_cast<std::size_t>(itemIndex) >= items_.size()) {
        return {};
    }
    const Item& item = items_[static_cast<std::size_t>(itemIndex)];
    if (item.id == 0) {
        return L"---";
    }
    if (textQuery_) {
        const std::wstring text = textQuery_(item.id);
        if (!text.empty()) {
            return text;
        }
    }
    return item.key;
}

std::wstring ToolBar::VisibleText() const {
    std::wstring out;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!out.empty()) {
            out += L" | ";
        }
        // 勾上的按钮前面标一个 [v]：屏幕上它是那块绿底 + 边框，读回来得有个对应物，
        // 否则"勾选态"这条链路的验证就只能靠人看
        if (CheckedAt(static_cast<int>(i))) {
            out += L"[v] ";
        }
        out += TextAt(static_cast<int>(i));
    }
    return out;
}

int ToolBar::HitTest(int x) const {
    if (x < 0) {
        return -1;
    }
    int left = Scale(4);
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const int width = items_[i].id == 0 ? SeparatorWidth() : CellSize();
        if (x >= left && x < left + width) {
            return items_[i].id == 0 ? -1 : static_cast<int>(i);   // 分隔符不接点击
        }
        left += width;
    }
    return -1;
}

void ToolBar::Invoke(int itemIndex) {
    if (!EnabledAt(itemIndex) || !invoke_) {
        return;
    }
    invoke_(items_[static_cast<std::size_t>(itemIndex)].id);
}

void ToolBar::UpdateTooltip(int itemIndex) {
    if (tooltip_ == nullptr) {
        return;
    }
    const std::wstring text = TextAt(itemIndex);
    if (itemIndex < 0 || text.empty()) {
        HideTooltip();
        return;
    }
    tipText_ = text;
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
    tool.hwnd = hwnd_;
    tool.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    tool.lpszText = tipText_.data();
    ::SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    POINT anchor{ItemLeft(static_cast<std::size_t>(itemIndex)) + CellSize() / 2, Height()};
    ::ClientToScreen(hwnd_, &anchor);
    ::SendMessageW(tooltip_, TTM_TRACKPOSITION, 0,
                   MAKELPARAM(anchor.x, anchor.y + Scale(2)));
    if (!tipShown_) {
        ::SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
        tipShown_ = true;
    }
}

void ToolBar::HideTooltip() {
    if (tooltip_ != nullptr && tipShown_) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
        tool.hwnd = hwnd_;
        tool.uId = reinterpret_cast<UINT_PTR>(hwnd_);
        tool.lpszText = tipText_.data();
        ::SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
    }
    tipShown_ = false;
}

void ToolBar::Paint(HDC dc) const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const Colors colors = PaletteOf(dark_, eyeCare_);

    HBRUSH background = ::CreateSolidBrush(colors.bg);
    ::FillRect(dc, &client, background);
    ::DeleteObject(background);

    if (font_ != nullptr) {
        ::SelectObject(dc, font_);
    }
    ::SetBkMode(dc, TRANSPARENT);

    for (std::size_t i = 0; i < items_.size(); ++i) {
        const int left = ItemLeft(i);
        if (items_[i].id == 0) {
            const int x = left + SeparatorWidth() / 2;
            RECT line{x, Scale(5), x + 1, client.bottom - Scale(5)};
            HBRUSH brush = ::CreateSolidBrush(colors.separator);
            ::FillRect(dc, &line, brush);
            ::DeleteObject(brush);
            continue;
        }
        RECT box{left, 0, left + CellSize(), client.bottom - 1};
        if (box.left > client.right) {
            break;   // 窗口太窄：后面的按钮画不下就不画（没有溢出菜单，见设计说明书 2.4 记的差异）
        }
        const int index = static_cast<int>(i);
        const bool enabled = EnabledAt(index);
        const bool checked = CheckedAt(index);
        const bool pressed = pressed_ == index;
        const bool hot = hot_ == index;
        // 悬停 / 按下 / 勾选统一画成圆角浅底（不再有硬边框）
        if (pressed || checked || hot) {
            RECT fill = box;
            ::InflateRect(&fill, -Scale(2), -Scale(3));
            const COLORREF colour = pressed ? colors.pressed
                                            : (checked ? colors.checked : colors.hot);
            FillRoundedRect(dc, fill, Scale(6), colour);
        }
        const int inset = (CellSize() - Scale(iconSize_)) / 2;
        RECT glyph{box.left + inset, box.top + inset, box.right - inset, box.bottom - inset};
        PaintGlyph(dc, glyph, items_[i].key,
                   enabled ? (checked ? colors.accent : colors.fg) : colors.disabled);
    }

    // 底下一条分隔线：工具栏和标签栏得有个分界，不然两块底色挨着看不清
    // （细线按 DPI 缩放，200% 下才不至于细成一根头发丝）
    const int hairline = Scale(1);
    RECT divider{0, client.bottom - hairline, client.right, client.bottom};
    HBRUSH brush = ::CreateSolidBrush(colors.separator);
    ::FillRect(dc, &divider, brush);
    ::DeleteObject(brush);
}

LRESULT CALLBACK ToolBar::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ToolBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ToolBar*>(create->lpCreateParams);
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

LRESULT ToolBar::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd_, &paint);
            Paint(dc);
            ::EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;   // 背景由 Paint 一次刷完，免得闪
        case WM_SIZE:
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
            const int index = HitTest(GET_X_LPARAM(lParam));
            if (index != hot_) {
                hot_ = index;
                UpdateTooltip(index);
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            tracking_ = false;
            hot_ = -1;
            HideTooltip();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int index = HitTest(GET_X_LPARAM(lParam));
            if (index < 0) {
                return 0;
            }
            pressed_ = index;
            ::SetCapture(hwnd_);
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const bool captured = ::GetCapture() == hwnd_;
            if (captured) {
                ::ReleaseCapture();
            }
            const int index = HitTest(GET_X_LPARAM(lParam));
            const int pressed = pressed_;
            pressed_ = -1;
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            // 按下与松开必须在同一个按钮上：和系统按钮一样的判定，
            // 拖出去再松手不该触发（否则"点错了想反悔"就没法子）
            if (captured && index >= 0 && index == pressed) {
                Invoke(index);
            }
            return 0;
        }
        case kMsgItemCount:
            return static_cast<LRESULT>(ButtonCount());
        case kMsgItemCenterX: {
            const int item = OrdinalItem(static_cast<int>(wParam));
            if (item < 0) {
                return -1;
            }
            return static_cast<LRESULT>(ItemLeft(static_cast<std::size_t>(item)) + CellSize() / 2);
        }
        case WM_GETTEXTLENGTH: {
            const std::wstring text = VisibleText();
            return static_cast<LRESULT>(text.size());
        }
        case WM_GETTEXT: {
            if (wParam == 0 || lParam == 0) {
                return 0;
            }
            const std::wstring text = VisibleText();
            const std::size_t capacity = static_cast<std::size_t>(wParam);
            const std::size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
            std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), text.c_str(), count);
            reinterpret_cast<wchar_t*>(lParam)[count] = L'\0';
            return static_cast<LRESULT>(count);
        }
        case WM_DESTROY:
            HideTooltip();
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::shell
