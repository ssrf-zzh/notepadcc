#include "shell/MacroDialog.h"

#include <commctrl.h>

#include <cwchar>
#include <vector>

namespace npcc::shell {
namespace {

constexpr const wchar_t* kNameClass = L"notepadccMacroName";
constexpr const wchar_t* kRunClass = L"notepadccMacroRun";

enum : int {
    kIdLabel = 1300,
    kIdEdit = 1301,
    kIdHint = 1302,
    kIdCheck = 1303,
    kIdOk = 1310,
    kIdCancel = 1311,
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

// 两个小对话框长得几乎一样（一行字、一个输入框、两个按钮），所以共用一套实现：
// "取名"与"运行选项"的区别只在提示文字、默认值、以及有没有那个复选框。
struct DialogSpec {
    const wchar_t* windowClass;
    const wchar_t* title;
    std::wstring   prompt;
    std::wstring   value;          // 输入框初值
    std::wstring   hint;           // 输入框下面的灰字说明（可空）
    bool           withCheckbox = false;
    const wchar_t* checkboxText = L"";
    bool           checkboxChecked = false;
    bool           numeric = false;   // 输入框只收数字
    int            maxLength = 64;
};

class SmallDialog {
public:
    bool Show(HWND owner, DialogSpec& spec);
    const std::wstring& Value() const { return spec_->value; }
    bool Checked() const { return checked_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);
    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    void Commit();

    HWND        owner_ = nullptr;
    HWND        hwnd_ = nullptr;
    HFONT       font_ = nullptr;
    DialogSpec* spec_ = nullptr;
    bool        checked_ = false;
    bool        done_ = false;
    bool        accepted_ = false;
};

int SmallDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND SmallDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void SmallDialog::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(10);
    const int row = Scale(24);
    const int buttonWidth = Scale(84);
    const int buttonHeight = Scale(28);
    const int width = client.right - client.left;

    int y = gap;
    if (HWND label = ::GetDlgItem(hwnd_, kIdLabel); label != nullptr) {
        ::MoveWindow(label, gap, y, width - gap * 2, row, TRUE);
    }
    y += row;
    if (HWND edit = ::GetDlgItem(hwnd_, kIdEdit); edit != nullptr) {
        ::MoveWindow(edit, gap, y, width - gap * 2, row, TRUE);
    }
    y += row + Scale(2);
    if (HWND hint = ::GetDlgItem(hwnd_, kIdHint); hint != nullptr) {
        ::MoveWindow(hint, gap, y, width - gap * 2, row, TRUE);
        y += row;
    }
    if (HWND check = ::GetDlgItem(hwnd_, kIdCheck); check != nullptr) {
        ::MoveWindow(check, gap, y, width - gap * 2, row, TRUE);
    }
    const int buttonY = client.bottom - buttonHeight - gap;
    int right = width - gap - buttonWidth;
    if (HWND ok = ::GetDlgItem(hwnd_, kIdOk); ok != nullptr) {
        ::MoveWindow(ok, right, buttonY, buttonWidth, buttonHeight, TRUE);
        right -= buttonWidth + Scale(6);
    }
    if (HWND cancel = ::GetDlgItem(hwnd_, kIdCancel); cancel != nullptr) {
        ::MoveWindow(cancel, right, buttonY, buttonWidth, buttonHeight, TRUE);
    }
}

void SmallDialog::Commit() {
    HWND edit = ::GetDlgItem(hwnd_, kIdEdit);
    const int length = edit != nullptr
                           ? static_cast<int>(::SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0))
                           : 0;
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (edit != nullptr) {
        ::GetWindowTextW(edit, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    // 前后的空白不该算进名字/次数里（" Macro 1 " 与 "Macro 1" 是同一个宏）
    const std::size_t first = text.find_first_not_of(L" \t");
    const std::size_t last = text.find_last_not_of(L" \t");
    spec_->value = first == std::wstring::npos ? std::wstring() : text.substr(first, last - first + 1);
    if (HWND check = ::GetDlgItem(hwnd_, kIdCheck); check != nullptr) {
        checked_ = ::SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
}

bool SmallDialog::Show(HWND owner, DialogSpec& spec) {
    owner_ = owner;
    spec_ = &spec;
    checked_ = spec.checkboxChecked;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SmallDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = spec.windowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(380);
    const int height = Scale(spec.withCheckbox ? 190 : 165);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, spec.windowClass, spec.title,
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

    Make(L"STATIC", spec.prompt.c_str(), SS_LEFT, kIdLabel);
    HWND edit = Make(L"EDIT", spec.value.c_str(),
                     WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL |
                         (spec.numeric ? ES_NUMBER : 0),
                     kIdEdit);
    if (edit != nullptr) {
        ::SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(spec.maxLength), 0);
        ::SendMessageW(edit, EM_SETSEL, 0, -1);   // 初值全选，直接输入即可替换
    }
    if (!spec.hint.empty()) {
        Make(L"STATIC", spec.hint.c_str(), SS_LEFT, kIdHint);
    }
    if (spec.withCheckbox) {
        HWND check = Make(L"BUTTON", spec.checkboxText, WS_TABSTOP | BS_AUTOCHECKBOX, kIdCheck);
        if (check != nullptr && spec.checkboxChecked) {
            ::SendMessageW(check, BM_SETCHECK, BST_CHECKED, 0);
        }
    }
    Make(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, kIdOk);
    Make(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);
    (void)::SendMessageW(hwnd_, DM_SETDEFID, kIdOk, 0);

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
    ::SetFocus(edit);

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
    return accepted_;
}

LRESULT CALLBACK SmallDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SmallDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SmallDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT SmallDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kIdOk) {
                Commit();
                accepted_ = true;
                done_ = true;
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

}  // namespace

bool AskMacroName(HWND owner, const std::wstring& suggested, std::wstring& out) {
    DialogSpec spec;
    spec.windowClass = kNameClass;
    spec.title = L"保存宏";
    spec.prompt = L"给这段录制起个名字（同名会覆盖已有的宏）：";
    spec.value = suggested;
    spec.hint = L"名字在宏菜单里显示，并且可以绑快捷键。";
    spec.maxLength = 40;
    SmallDialog dialog;
    if (!dialog.Show(owner, spec)) {
        return false;
    }
    if (spec.value.empty()) {
        // 空名字没法在菜单里区分，也不能当标识用
        ::MessageBoxW(owner, L"名字不能是空的。", L"保存宏", MB_ICONWARNING);
        return false;
    }
    out = spec.value;
    return true;
}

bool AskRunCommandName(HWND owner, const std::wstring& suggested, std::wstring& out) {
    DialogSpec spec;
    spec.windowClass = kNameClass;
    spec.title = L"保存运行命令";
    spec.prompt = L"给这条命令起个名字（同名会覆盖已有的命令）：";
    spec.value = suggested;
    spec.hint = L"名字会出现在「运行」菜单里，也可以像别的命令一样绑快捷键。";
    spec.maxLength = 60;
    SmallDialog dialog;
    if (!dialog.Show(owner, spec)) {
        return false;
    }
    if (spec.value.empty()) {
        ::MessageBoxW(owner, L"名字不能是空的。", L"保存运行命令", MB_ICONWARNING);
        return false;
    }
    out = spec.value;
    return true;
}

bool AskMacroRunOptions(HWND owner, const std::wstring& macroName, MacroRunOptions& out) {
    DialogSpec spec;
    spec.windowClass = kRunClass;
    spec.title = L"执行宏";
    spec.prompt = L"宏「" + macroName + L"」，重复执行次数：";
    spec.value = std::to_wstring(out.repeatCount);
    spec.hint = L"1 ~ 10000。次数越多越慢，可随时按停止中断。";
    spec.withCheckbox = true;
    spec.checkboxText = L"跳过失败步骤继续（宽松模式）";
    spec.checkboxChecked = !out.stopOnError;
    spec.numeric = true;
    spec.maxLength = 5;
    SmallDialog dialog;
    if (!dialog.Show(owner, spec)) {
        return false;
    }
    const int count = ::_wtoi(spec.value.c_str());
    if (count < 1 || count > 10000) {
        ::MessageBoxW(owner, L"重复次数要在 1 ~ 10000 之间。", L"执行宏", MB_ICONWARNING);
        return false;
    }
    out.repeatCount = count;
    out.stopOnError = !dialog.Checked();
    return true;
}

}  // namespace npcc::shell
