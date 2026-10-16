#include "shell/ComparePickDialog.h"

#include <commctrl.h>

#include <cwchar>

#include "core/Localization.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccComparePick";

enum : int {
    kIdPrompt = 1500,
    kIdList   = 1501,
    kIdOk     = 1510,
    kIdCancel = 1511,
};

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

class ComparePickDialog {
public:
    bool Show(HWND owner, const std::vector<std::wstring>& names,
              const std::wstring& currentName, std::size_t& out);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    void Accept();

    HWND                       owner_ = nullptr;
    HWND                       hwnd_ = nullptr;
    HWND                       list_ = nullptr;
    HFONT                      font_ = nullptr;
    const std::vector<std::wstring>* names_ = nullptr;
    std::size_t                selection_ = 0;
    bool                       done_ = false;
    bool                       accepted_ = false;
};

int ComparePickDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND ComparePickDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void ComparePickDialog::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(12);
    const int row = Scale(22);
    const int buttonWidth = Scale(90);
    const int buttonHeight = Scale(28);
    const int width = client.right - client.left;

    if (HWND prompt = ::GetDlgItem(hwnd_, kIdPrompt); prompt != nullptr) {
        ::MoveWindow(prompt, gap, gap, width - gap * 2, row, TRUE);
    }
    const int listTop = gap + row;
    const int buttonY = client.bottom - buttonHeight - gap;
    if (list_ != nullptr) {
        ::MoveWindow(list_, gap, listTop, width - gap * 2, buttonY - listTop - gap, TRUE);
    }
    int right = width - gap - buttonWidth;
    for (const int id : {kIdOk, kIdCancel}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, right, buttonY, buttonWidth, buttonHeight, TRUE);
            right -= buttonWidth + Scale(6);
        }
    }
}

void ComparePickDialog::Accept() {
    if (list_ == nullptr) {
        return;
    }
    const LRESULT index = ::SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR) {
        return;   // 没选就不关：让用户选一个
    }
    selection_ = static_cast<std::size_t>(index);
    accepted_ = true;
    done_ = true;
}

LRESULT CALLBACK ComparePickDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam) {
    auto* self = reinterpret_cast<ComparePickDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ComparePickDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT ComparePickDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kIdList && notification == LBN_DBLCLK) {
                Accept();   // 双击 = 确定
                return 0;
            }
            if (id == kIdOk) {
                Accept();
                return 0;
            }
            if (id == kIdCancel) {
                done_ = true;
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;
                return 0;
            }
            break;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_CLOSE:
            done_ = true;
            return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool ComparePickDialog::Show(HWND owner, const std::vector<std::wstring>& names,
                             const std::wstring& currentName, std::size_t& out) {
    owner_ = owner;
    names_ = &names;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ComparePickDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(420);
    const int height = Scale(300);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    const std::wstring title = core::Tr(L"compare.pick.title", L"选择要比较的文档");
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, title.c_str(),
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner,
                              nullptr, ::GetModuleHandleW(nullptr), this);
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

    const std::wstring prompt =
        core::Tr(L"compare.pick.prompt", L"当前文档：") + currentName +
        core::Tr(L"compare.pick.promptTail", L"　——　跟哪一份比？");
    Make(L"STATIC", prompt.c_str(), WS_VISIBLE | SS_LEFT, kIdPrompt);
    list_ = Make(WC_LISTBOXW, L"",
                 WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY, kIdList);
    if (list_ != nullptr) {
        for (const std::wstring& name : names) {
            ::SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        ::SendMessageW(list_, LB_SETCURSEL, 0, 0);
    }
    const std::wstring ok = core::Tr(L"compare.pick.ok", L"比较(&C)");
    const std::wstring cancel = core::Tr(L"prefs.cancel", L"取消");
    Make(L"BUTTON", ok.c_str(), WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdOk);
    Make(L"BUTTON", cancel.c_str(), WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    Layout();

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    Layout();
    if (list_ != nullptr) {
        ::SetFocus(list_);
    }

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        if (::IsDialogMessageW(hwnd_, &message) == 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    ::EnableWindow(owner_, TRUE);
    ::SetForegroundWindow(owner_);
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (::IsWindow(hwnd_) != 0) {
        ::DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    list_ = nullptr;
    if (accepted_) {
        out = selection_;
    }
    return accepted_;
}

}  // namespace

bool PickDocumentToCompare(HWND owner, const std::vector<std::wstring>& names,
                           const std::wstring& currentName, std::size_t& out) {
    ComparePickDialog dialog;
    return dialog.Show(owner, names, currentName, out);
}

}  // namespace npcc::shell
