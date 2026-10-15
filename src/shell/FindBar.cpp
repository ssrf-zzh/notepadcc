#include "shell/FindBar.h"

#include <commctrl.h>

#include <algorithm>

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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
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
                                 WS_VSCROLL,
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
}

HWND FindBar::MakeControl(const wchar_t* className, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, className, text, style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void FindBar::ApplyFont() {
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
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
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
    const int rows = replaceMode_ ? 2 : 1;
    return rows * Scale(kRowHeight) + Scale(kGap) * (rows + 1);
}

void FindBar::Layout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int rowHeight = Scale(kRowHeight);
    const int gap = Scale(kGap);
    const int row1 = gap;
    const int row2 = gap * 2 + rowHeight;

    int x = gap;
    auto place = [&](HWND control, int w, int y, int h) {
        if (control == nullptr) {
            return;
        }
        ::MoveWindow(control, x, y, w, h, TRUE);
        x += w + Scale(4);
    };

    // 第一行：查找输入 + 模式 + 选项 + 操作
    const int buttonWidth = Scale(64);
    const int checkWidth = Scale(86);
    int reserved = buttonWidth * 5 + checkWidth * 3 + gap * 12 + Scale(120);
    int findWidth = std::max(Scale(120), width - reserved);
    place(findEdit_, findWidth, row1, rowHeight);
    place(modeCombo_, Scale(76), row1, rowHeight * 6);   // 下拉列表要高一些
    place(::GetDlgItem(hwnd_, kCaseCheck), checkWidth, row1, rowHeight);
    place(::GetDlgItem(hwnd_, kWordCheck), checkWidth, row1, rowHeight);
    place(::GetDlgItem(hwnd_, kDotAllCheck), checkWidth, row1, rowHeight);
    place(::GetDlgItem(hwnd_, kFindPrevButton), buttonWidth, row1, rowHeight);
    place(::GetDlgItem(hwnd_, kFindNextButton), buttonWidth, row1, rowHeight);
    place(::GetDlgItem(hwnd_, kCountButton), buttonWidth, row1, rowHeight);
    place(::GetDlgItem(hwnd_, kMarkButton), buttonWidth, row1, rowHeight);
    place(::GetDlgItem(hwnd_, kCloseButton), buttonWidth, row1, rowHeight);

    if (replaceMode_) {
        x = gap;
        place(replaceEdit_, findWidth, row2, rowHeight);
        place(::GetDlgItem(hwnd_, kReplaceButton), buttonWidth, row2, rowHeight);
        place(::GetDlgItem(hwnd_, kReplaceAllButton), buttonWidth * 3 / 2, row2, rowHeight);
    }

    // 状态文字放在第一行右侧空白处
    if (statusText_ != nullptr) {
        const int statusWidth = Scale(220);
        ::MoveWindow(statusText_, std::max(gap, width - statusWidth - gap), row1,
                     statusWidth, rowHeight, TRUE);
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

std::wstring FindBar::FindText() const {
    return TextOf(kFindEdit);
}

void FindBar::SetFindText(const std::wstring& text) {
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
        case WM_SIZE:
            Layout();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_COMMAND:
            // 控件事件上抛给主窗口统一处理
            ::SendMessageW(::GetParent(hwnd_), WM_COMMAND, wParam, lParam);
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
