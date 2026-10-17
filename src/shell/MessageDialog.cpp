#include "shell/MessageDialog.h"

#include <algorithm>
#include <string>
#include <vector>

#include "core/Localization.h"
#include "shell/ChromeStyle.h"

namespace npcc::shell {
namespace {

constexpr int kTextId = 100;
constexpr int kIconWidth = 44;
constexpr int kMinButtonWidth = 88;

int DpiOf(HWND reference) {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr && reference != nullptr) {
            const UINT actual = getDpi(reference);
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return dpi == 0 ? 96 : static_cast<int>(dpi);
}

struct ButtonSpec {
    int          id = IDOK;
    std::wstring label;
};

std::vector<ButtonSpec> ButtonsFor(UINT type, int* defaultId, int* escapeId) {
    std::vector<ButtonSpec> buttons;
    const std::wstring ok = npcc::core::Tr(L"prefs.ok", L"确定");
    const std::wstring cancel = npcc::core::Tr(L"prefs.cancel", L"取消");
    const std::wstring yes = npcc::core::Tr(L"dialog.common.yes", L"是");
    const std::wstring no = npcc::core::Tr(L"dialog.common.no", L"否");
    switch (type & 0x0F) {
        case MB_YESNOCANCEL:
            buttons = {{IDYES, yes}, {IDNO, no}, {IDCANCEL, cancel}};
            *defaultId = IDYES;
            *escapeId = IDCANCEL;
            break;
        case MB_YESNO:
            buttons = {{IDYES, yes}, {IDNO, no}};
            *defaultId = IDYES;
            *escapeId = IDNO;
            break;
        default:
            buttons = {{IDOK, ok}};
            *defaultId = IDOK;
            *escapeId = IDOK;
            break;
    }
    if ((type & MB_DEFMASK) == MB_DEFBUTTON2 && buttons.size() > 1) {
        *defaultId = buttons[1].id;
    } else if ((type & MB_DEFMASK) == MB_DEFBUTTON3 && buttons.size() > 2) {
        *defaultId = buttons[2].id;
    }
    return buttons;
}

const wchar_t* IconResource(UINT type) {
    switch (type & MB_ICONMASK) {
        case MB_ICONERROR:
            return IDI_ERROR;
        case MB_ICONWARNING:
            return IDI_WARNING;
        case MB_ICONQUESTION:
            return IDI_QUESTION;
        case MB_ICONINFORMATION:
            return IDI_INFORMATION;
        default:
            return nullptr;
    }
}

class MessageDialog {
public:
    int Show(HWND owner, const wchar_t* text, const wchar_t* title, UINT type);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    void Finish(int result);

    HWND                  hwnd_ = nullptr;
    HWND                  owner_ = nullptr;
    HFONT                 font_ = nullptr;
    HICON                 icon_ = nullptr;
    HWND                  text_ = nullptr;
    std::vector<HWND>     buttons_;
    std::vector<int>      buttonIds_;
    std::vector<int>      buttonWidths_;
    int                   defaultId_ = IDOK;
    int                   escapeId_ = IDOK;
    int                   result_ = IDOK;
    bool                  done_ = false;
    bool                  ownerWasEnabled_ = true;
    int                   dpi_ = 96;
};

int MessageDialog::Scale(int value) const {
    return ::MulDiv(value, dpi_, 96);
}

HWND MessageDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND control = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                     hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     ::GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr && font_ != nullptr) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    return control;
}

int MessageDialog::Show(HWND owner, const wchar_t* text, const wchar_t* title, UINT type) {
    owner_ = owner != nullptr ? owner : ::GetActiveWindow();
    dpi_ = DpiOf(owner_);
    ownerWasEnabled_ = owner_ == nullptr || ::IsWindowEnabled(owner_) != 0;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MessageDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMessageDialogClass;
    ::RegisterClassExW(&wc);

    font_ = CreateChromeFont(dpi_, 90, false);
    const wchar_t* iconName = IconResource(type);
    if (iconName != nullptr) {
        icon_ = ::LoadIconW(nullptr, iconName);
    }

    std::wstring body = text != nullptr ? text : L"";
    std::wstring caption = title != nullptr && title[0] != L'\0' ? title : L"notepadcc";

    int defaultId = IDOK;
    int escapeId = IDOK;
    const std::vector<ButtonSpec> specs = ButtonsFor(type, &defaultId, &escapeId);
    defaultId_ = defaultId;
    escapeId_ = escapeId;

    const int margin = Scale(20);
    const int gap = Scale(12);
    const int iconWidth = icon_ != nullptr ? Scale(kIconWidth) : 0;
    const int buttonHeight = Scale(30);
    const int buttonBottom = Scale(18);
    const int minWidth = Scale(360);
    const int maxWidth = Scale(620);

    int textWidth = 0;
    int textHeight = 0;
    HDC measureDc = ::GetDC(owner_);
    if (measureDc != nullptr) {
        const HGDIOBJ oldFont = font_ != nullptr ? ::SelectObject(measureDc, font_) : nullptr;
        RECT measure{0, 0, maxWidth - margin * 2 - iconWidth - gap, 0};
        ::DrawTextW(measureDc, body.c_str(), static_cast<int>(body.size()), &measure,
                    DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
        textWidth = std::max(Scale(220), static_cast<int>(measure.right - measure.left));
        textHeight = std::max(Scale(20), static_cast<int>(measure.bottom - measure.top));
        if (oldFont != nullptr) {
            ::SelectObject(measureDc, oldFont);
        }
        ::ReleaseDC(owner_, measureDc);
    }
    if (textWidth == 0) {
        textWidth = Scale(320);
        textHeight = Scale(40);
    }

    int totalButtonWidth = 0;
    buttonWidths_.clear();
    buttonWidths_.reserve(specs.size());
    for (const ButtonSpec& spec : specs) {
        int width = Scale(kMinButtonWidth);
        HDC dc = ::GetDC(owner_);
        if (dc != nullptr) {
            const HGDIOBJ oldFont = font_ != nullptr ? ::SelectObject(dc, font_) : nullptr;
            SIZE size{};
            ::GetTextExtentPoint32W(dc, spec.label.c_str(), static_cast<int>(spec.label.size()),
                                    &size);
            width = std::max(width, static_cast<int>(size.cx) + Scale(32));
            if (oldFont != nullptr) {
                ::SelectObject(dc, oldFont);
            }
            ::ReleaseDC(owner_, dc);
        }
        buttonWidths_.push_back(width);
        totalButtonWidth += width;
    }
    if (!buttonWidths_.empty()) {
        totalButtonWidth += gap * static_cast<int>(buttonWidths_.size() - 1);
    }

    const int contentWidth = std::max(textWidth + iconWidth + gap, totalButtonWidth);
    const int width = std::max(minWidth, std::min(maxWidth, contentWidth + margin * 2));
    const int height = margin + textHeight + Scale(18) + buttonHeight + buttonBottom;

    RECT anchorRect{};
    if (owner_ != nullptr) {
        ::GetWindowRect(owner_, &anchorRect);
    } else {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchorRect, 0);
    }
    const int x = anchorRect.left + ((anchorRect.right - anchorRect.left) - width) / 2;
    const int y = anchorRect.top + ((anchorRect.bottom - anchorRect.top) - height) / 2;

    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kMessageDialogClass, caption.c_str(),
                              WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, width,
                              height, owner_,
                              nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        if (font_ != nullptr) {
            ::DeleteObject(font_);
            font_ = nullptr;
        }
        return IDCANCEL;
    }

    text_ = Make(L"STATIC", body.c_str(), SS_LEFT | SS_NOPREFIX, kTextId);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const DWORD style = specs[i].id == defaultId_ ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON;
        HWND button = Make(L"BUTTON", specs[i].label.c_str(), WS_TABSTOP | style, specs[i].id);
        buttons_.push_back(button);
        buttonIds_.push_back(specs[i].id);
    }

    ApplyDialogChrome(hwnd_);
    Layout();

    if (owner_ != nullptr && ownerWasEnabled_) {
        ::EnableWindow(owner_, FALSE);
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    ::SetForegroundWindow(hwnd_);
    for (std::size_t i = 0; i < buttonIds_.size(); ++i) {
        if (buttonIds_[i] == defaultId_ && buttons_[i] != nullptr) {
            ::SetFocus(buttons_[i]);
            break;
        }
    }

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            Finish(escapeId_);
            continue;
        }
        if (::IsDialogMessageW(hwnd_, &message) == 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    if (owner_ != nullptr && ownerWasEnabled_) {
        ::EnableWindow(owner_, TRUE);
        ::SetForegroundWindow(owner_);
    }
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (::IsWindow(hwnd_) != 0) {
        ::DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    return result_;
}

void MessageDialog::Layout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = Scale(20);
    const int gap = Scale(12);
    const int iconWidth = icon_ != nullptr ? Scale(kIconWidth) : 0;
    const int buttonHeight = Scale(30);
    const int buttonBottom = Scale(18);

    const int textLeft = margin + iconWidth + (iconWidth > 0 ? gap : 0);
    const int textWidth = std::max(Scale(50), width - textLeft - margin);
    if (text_ != nullptr) {
        ::MoveWindow(text_, textLeft, margin, textWidth,
                     std::max(Scale(20), height - margin - buttonHeight - buttonBottom - Scale(18)),
                     TRUE);
    }

    int totalWidth = 0;
    for (const int buttonWidth : buttonWidths_) {
        totalWidth += buttonWidth;
    }
    if (!buttonWidths_.empty()) {
        totalWidth += gap * static_cast<int>(buttonWidths_.size() - 1);
    }
    int x = std::max(margin, width - margin - totalWidth);
    const int y = height - buttonBottom - buttonHeight;
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const int buttonWidth = i < buttonWidths_.size()
                                    ? buttonWidths_[i]
                                    : std::max(Scale(kMinButtonWidth), buttonHeight * 3);
        ::MoveWindow(buttons_[i], x, y, buttonWidth, buttonHeight, TRUE);
        x += buttonWidth + gap;
    }
}

void MessageDialog::Finish(int result) {
    result_ = result;
    done_ = true;
}

LRESULT CALLBACK MessageDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    auto* self = reinterpret_cast<MessageDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MessageDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT MessageDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd_, &paint);
            RECT client{};
            ::GetClientRect(hwnd_, &client);
            const HBRUSH background = ::CreateSolidBrush(CurrentChromeColours().strip);
            ::FillRect(dc, &client, background);
            ::DeleteObject(background);
            if (icon_ != nullptr) {
                const int size = Scale(32);
                ::DrawIconEx(dc, Scale(20), Scale(24), icon_, size, size, 0, nullptr, DI_NORMAL);
            }
            ::EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_COMMAND:
            Finish(static_cast<int>(LOWORD(wParam)));
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) {
                Finish(defaultId_);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                Finish(escapeId_);
                return 0;
            }
            break;
        case WM_CLOSE:
            Finish(escapeId_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace

int ShowMessageBox(HWND owner, const wchar_t* text, const wchar_t* title, UINT type) {
    MessageDialog dialog;
    return dialog.Show(owner, text, title, type);
}

}  // namespace npcc::shell
