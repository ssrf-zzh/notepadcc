#include "shell/RunCommandDialog.h"

#include <commctrl.h>

#include <cwchar>

#include "shell/ChromeStyle.h"
#include "shell/MessageDialog.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccRunCommand";

enum : int {
    kIdLabel   = 1400,
    kIdCombo   = 1401,
    kIdCapture = 1402,
    kIdHint    = 1403,
    kIdGo      = 1410,
    kIdSave    = 1411,
    kIdCancel  = 1412,
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

std::wstring Trim(std::wstring text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t' ||
                             text.front() == L'\r' || text.front() == L'\n')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' ||
                             text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return text;
}

class RunCommandDialog {
public:
    bool Show(HWND owner, const std::vector<core::RunCommandDef>& savedCommands,
              const std::wstring& initial, RunDialogResult& out);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    std::wstring CommandText() const;
    void Accept(RunDialogAction action);

    HWND        owner_ = nullptr;
    HWND        hwnd_ = nullptr;
    HFONT       font_ = nullptr;
    bool        done_ = false;
    bool        accepted_ = false;
    RunDialogResult result_;
};

int RunCommandDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND RunCommandDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

std::wstring RunCommandDialog::CommandText() const {
    HWND combo = ::GetDlgItem(hwnd_, kIdCombo);
    if (combo == nullptr) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(combo);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int written = ::GetWindowTextW(combo, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(written < 0 ? 0 : written));
    return Trim(std::move(text));
}

void RunCommandDialog::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(10);
    const int row = Scale(24);
    const int buttonWidth = Scale(96);
    const int buttonHeight = Scale(28);
    const int width = client.right - client.left;
    const int inner = width - gap * 2;

    int y = gap;
    if (HWND label = ::GetDlgItem(hwnd_, kIdLabel); label != nullptr) {
        ::MoveWindow(label, gap, y, inner, row, TRUE);
    }
    y += row;
    if (HWND combo = ::GetDlgItem(hwnd_, kIdCombo); combo != nullptr) {
        // 下拉的高度是"列表展开时的高度"，所以给得比一行大得多
        ::MoveWindow(combo, gap, y, inner, row + Scale(180), TRUE);
    }
    y += row + Scale(4);
    if (HWND capture = ::GetDlgItem(hwnd_, kIdCapture); capture != nullptr) {
        ::MoveWindow(capture, gap, y, inner, row, TRUE);
    }
    const int buttonY = client.bottom - buttonHeight - gap;
    int right = width - gap - buttonWidth;
    for (const int id : {kIdGo, kIdSave, kIdCancel}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, right, buttonY, buttonWidth, buttonHeight, TRUE);
            right -= buttonWidth + gap / 2;
        }
    }
    if (HWND hint = ::GetDlgItem(hwnd_, kIdHint); hint != nullptr) {
        // 剩下的地方全给提示文字（多行）
        const int top = y + row;
        const int height = buttonY - top - gap;
        ::MoveWindow(hint, gap, top, inner, height > row ? height : row, TRUE);
    }
}

void RunCommandDialog::Accept(RunDialogAction action) {
    const std::wstring command = CommandText();
    if (command.empty()) {
        ShowMessageBox(hwnd_, L"命令行是空的，先写一条再运行。", L"运行命令", MB_ICONINFORMATION);
        return;
    }
    result_.action = action;
    result_.command = command;
    result_.captureOutput = ::IsDlgButtonChecked(hwnd_, kIdCapture) == BST_CHECKED;
    if (action == RunDialogAction::Save) {
        std::wstring name;
        if (!AskRunCommandName(hwnd_, command, name)) {
            return;   // 用户取消取名：对话框留着，别把已经敲好的命令弄丢
        }
        result_.name = std::move(name);
    }
    accepted_ = true;
    done_ = true;
}

LRESULT CALLBACK RunCommandDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                                LPARAM lParam) {
    auto* self = reinterpret_cast<RunCommandDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RunCommandDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT RunCommandDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kIdCombo && notification == CBN_SELCHANGE) {
                // 选中一条已保存的命令：把它的命令行连同"捕获输出"设置一起填回来
                HWND combo = ::GetDlgItem(hwnd_, kIdCombo);
                const int index = combo != nullptr
                                      ? static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0))
                                      : -1;
                if (combo != nullptr && index >= 0) {
                    const LRESULT data =
                        ::SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
                    const auto* def = reinterpret_cast<const core::RunCommandDef*>(
                        static_cast<INT_PTR>(data));
                    if (def != nullptr) {
                        ::SetWindowTextW(combo, def->command.c_str());
                        ::CheckDlgButton(hwnd_, kIdCapture,
                                         def->captureOutput ? BST_CHECKED : BST_UNCHECKED);
                    }
                }
                return 0;
            }
            switch (id) {
                case kIdGo:
                    Accept(RunDialogAction::Run);
                    return 0;
                case kIdSave:
                    Accept(RunDialogAction::Save);
                    return 0;
                case kIdCancel:
                    done_ = true;
                    return 0;
                default:
                    break;
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

bool RunCommandDialog::Show(HWND owner, const std::vector<core::RunCommandDef>& savedCommands,
                            const std::wstring& initial, RunDialogResult& out) {
    owner_ = owner;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &RunCommandDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(620);
    const int height = Scale(330);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"运行命令",
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

    Make(L"STATIC", L"命令行（支持 $(FULL_CURRENT_PATH) 这类变量）：", WS_VISIBLE | SS_LEFT,
         kIdLabel);
    HWND combo = Make(WC_COMBOBOXW, initial.c_str(),
                      WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL | WS_BORDER,
                      kIdCombo);
    if (combo != nullptr) {
        for (const core::RunCommandDef& def : savedCommands) {
            const LRESULT index = ::SendMessageW(combo, CB_ADDSTRING, 0,
                                                 reinterpret_cast<LPARAM>(def.name.c_str()));
            if (index >= 0) {
                // 把整条定义的地址挂在列表项上：选中时要用它的命令行与捕获设置。
                // savedCommands 的生命周期覆盖整个对话框（调用方持有），所以只存指针
                ::SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index),
                               static_cast<LPARAM>(reinterpret_cast<INT_PTR>(&def)));
            }
        }
        ::SetWindowTextW(combo, initial.c_str());
    }
    Make(L"BUTTON", L"把输出收进一个新文档", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
         kIdCapture);
    ::CheckDlgButton(hwnd_, kIdCapture, BST_CHECKED);
    // 提示文字要写全：用户不看文档也知道 .bat 得走 cmd /c、工作目录在哪
    Make(L"STATIC",
         L"可用变量：$(FULL_CURRENT_PATH) $(CURRENT_DIRECTORY) $(FILE_NAME) $(NAME_PART) "
         L"$(EXT_PART) $(CURRENT_WORD) $(CURRENT_LINE) $(APP_DIRECTORY) "
         L"$(CURRENT_ENCODING) $(CURRENT_EOL)\r\n"
         L"工作目录：当前文件所在目录（未保存的文档用程序目录）。\r\n"
         L"要跑 .bat / .cmd 请写成 cmd /c xxx.bat；不勾选上面的框就直接跑，输出不进编辑器。",
         WS_VISIBLE | SS_LEFT, kIdHint);
    Make(L"BUTTON", L"运行(&R)", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdGo);
    Make(L"BUTTON", L"保存为命令(&S)...", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdSave);
    Make(L"BUTTON", L"取消", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    ApplyDialogChrome(hwnd_);
    Layout();

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    Layout();
    if (combo != nullptr) {
        ::SetFocus(combo);
    }

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        // IsDialogMessage 让 Tab/回车/方向键按系统的对话框语义走（回车 = 默认按钮）
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
    if (accepted_) {
        out = result_;
    }
    return accepted_;
}

}  // namespace

bool ShowRunCommandDialog(HWND owner, const std::vector<core::RunCommandDef>& savedCommands,
                          const std::wstring& initial, RunDialogResult& out) {
    RunCommandDialog dialog;
    return dialog.Show(owner, savedCommands, initial, out);
}

}  // namespace npcc::shell
