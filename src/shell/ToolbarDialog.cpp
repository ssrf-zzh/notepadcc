#include "shell/ToolbarDialog.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>

#include "core/CommandText.h"
#include "core/Localization.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccToolbarCustomize";

enum : int {
    kIdAvailableLabel = 1600,
    kIdAvailable      = 1601,
    kIdCurrentLabel   = 1602,
    kIdCurrent        = 1603,
    kIdAdd            = 1610,
    kIdRemove         = 1611,
    kIdUp             = 1612,
    kIdDown           = 1613,
    kIdSeparator      = 1614,
    kIdReset          = 1615,
    kIdOk             = 1620,
    kIdCancel         = 1621,
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

// 菜单里类别的排列顺序（与 MenuBuilder 那张表同一个意思：定制界面按它分组，
// 用户找命令时的顺序与菜单里一致）
const core::CmdCategory kOrder[] = {
    core::CmdCategory::File,  core::CmdCategory::Edit,    core::CmdCategory::Search,
    core::CmdCategory::View,  core::CmdCategory::Format,  core::CmdCategory::Encoding,
    core::CmdCategory::Tools, core::CmdCategory::Settings, core::CmdCategory::Window,
    core::CmdCategory::Macro, core::CmdCategory::Run,     core::CmdCategory::Plugin,
    core::CmdCategory::Help,
};

class ToolbarDialog {
public:
    bool Show(HWND owner, const core::CommandRegistry& registry,
              const core::ToolbarItems& current, ToolbarPicks& out);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    void FillLists();
    void AddSelected();
    void RemoveSelected();
    void MoveSelected(int delta);
    void Accept();
    std::wstring TextOfKey(const std::wstring& key) const;
    std::wstring AvailableText(int index) const;

    HWND                            owner_ = nullptr;
    HWND                            hwnd_ = nullptr;
    HWND                            availableBox_ = nullptr;
    HWND                            currentBox_ = nullptr;
    HFONT                           font_ = nullptr;
    const core::CommandRegistry*    registry_ = nullptr;
    std::vector<std::wstring>       available_;   // 还没有放上去的命令键
    core::ToolbarItems              current_;     // 现在的按钮表（含分隔符 "-"）
    bool                            useDefault_ = false;
    bool                            done_ = false;
    bool                            accepted_ = false;
};

int ToolbarDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND ToolbarDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

std::wstring ToolbarDialog::TextOfKey(const std::wstring& key) const {
    if (key == core::kToolbarSeparator) {
        return core::Tr(L"dialog.toolbar.separator", L"―――― 分隔符 ――――");
    }
    if (registry_ != nullptr) {
        if (const core::Command* command = registry_->FindByKey(key); command != nullptr) {
            return core::StripAccelerator(command->text);
        }
    }
    return key;
}

std::wstring ToolbarDialog::AvailableText(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= available_.size()) {
        return {};
    }
    const std::wstring& key = available_[static_cast<std::size_t>(index)];
    std::wstring text = TextOfKey(key);
    if (registry_ != nullptr) {
        if (const core::Command* command = registry_->FindByKey(key); command != nullptr) {
            // 带类别前缀：一百多条命令平铺着列，不分组名会很难找
            text = core::CategoryText(command->category) + L" - " + text;
        }
    }
    return text;
}

void ToolbarDialog::FillLists() {
    if (availableBox_ == nullptr || currentBox_ == nullptr) {
        return;
    }
    ::SendMessageW(availableBox_, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < available_.size(); ++i) {
        const std::wstring text = AvailableText(static_cast<int>(i));
        ::SendMessageW(availableBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    ::SendMessageW(currentBox_, LB_RESETCONTENT, 0, 0);
    for (const std::wstring& key : current_) {
        const std::wstring text = TextOfKey(key);
        ::SendMessageW(currentBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void ToolbarDialog::AddSelected() {
    const LRESULT index = ::SendMessageW(availableBox_, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR || static_cast<std::size_t>(index) >= available_.size()) {
        return;
    }
    current_.push_back(available_[static_cast<std::size_t>(index)]);
    available_.erase(available_.begin() + index);
    FillLists();
    ::SendMessageW(currentBox_, LB_SETCURSEL,
                   static_cast<WPARAM>(current_.size() - 1), 0);
}

void ToolbarDialog::RemoveSelected() {
    const LRESULT index = ::SendMessageW(currentBox_, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR || static_cast<std::size_t>(index) >= current_.size()) {
        return;
    }
    const std::wstring key = current_[static_cast<std::size_t>(index)];
    current_.erase(current_.begin() + index);
    if (key != core::kToolbarSeparator) {
        available_.push_back(key);
        std::sort(available_.begin(), available_.end());
    }
    FillLists();
}

void ToolbarDialog::MoveSelected(int delta) {
    const LRESULT index = ::SendMessageW(currentBox_, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR) {
        return;
    }
    const int from = static_cast<int>(index);
    const int to = from + delta;
    if (to < 0 || static_cast<std::size_t>(to) >= current_.size()) {
        return;
    }
    std::swap(current_[static_cast<std::size_t>(from)], current_[static_cast<std::size_t>(to)]);
    FillLists();
    ::SendMessageW(currentBox_, LB_SETCURSEL, static_cast<WPARAM>(to), 0);
}

void ToolbarDialog::Accept() {
    useDefault_ = false;
    accepted_ = true;
    done_ = true;
}

LRESULT CALLBACK ToolbarDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    auto* self = reinterpret_cast<ToolbarDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ToolbarDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT ToolbarDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kIdAvailable && notification == LBN_DBLCLK) {
                AddSelected();
                return 0;
            }
            if (id == kIdCurrent && notification == LBN_DBLCLK) {
                RemoveSelected();
                return 0;
            }
            switch (id) {
                case kIdAdd:
                    AddSelected();
                    return 0;
                case kIdRemove:
                    RemoveSelected();
                    return 0;
                case kIdUp:
                    MoveSelected(-1);
                    return 0;
                case kIdDown:
                    MoveSelected(1);
                    return 0;
                case kIdSeparator: {
                    // 分隔符可以连着点两次再被 NormalizeToolbarItems 去掉，
                    // 但界面上允许先摆着——用户可能还要在两个分隔符之间塞按钮
                    current_.push_back(core::kToolbarSeparator);
                    FillLists();
                    ::SendMessageW(currentBox_, LB_SETCURSEL,
                                   static_cast<WPARAM>(current_.size() - 1), 0);
                    return 0;
                }
                case kIdReset:
                    useDefault_ = true;
                    accepted_ = true;
                    done_ = true;
                    return 0;
                case kIdOk:
                    Accept();
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

void ToolbarDialog::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(10);
    const int row = Scale(20);
    const int buttonWidth = Scale(120);
    const int buttonHeight = Scale(26);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int columnWidth = Scale(250);
    const int midX = gap + columnWidth + gap;
    const int listTop = gap + row;
    const int listBottom = height - buttonHeight - gap * 3;

    if (HWND label = ::GetDlgItem(hwnd_, kIdAvailableLabel); label != nullptr) {
        ::MoveWindow(label, gap, gap, columnWidth, row, TRUE);
    }
    if (HWND label = ::GetDlgItem(hwnd_, kIdCurrentLabel); label != nullptr) {
        ::MoveWindow(label, midX + buttonWidth + gap, gap, width - midX - buttonWidth - gap * 2,
                     row, TRUE);
    }
    if (availableBox_ != nullptr) {
        ::MoveWindow(availableBox_, gap, listTop, columnWidth, listBottom - listTop, TRUE);
    }
    if (currentBox_ != nullptr) {
        ::MoveWindow(currentBox_, midX + buttonWidth + gap, listTop,
                     width - midX - buttonWidth - gap * 2, listBottom - listTop, TRUE);
    }
    const int midButtonX = gap + columnWidth + (buttonWidth > 0 ? gap : gap);
    const int stackTop = listTop;
    const int stackStep = buttonHeight + Scale(6);
    int y = stackTop;
    for (const int id : {kIdAdd, kIdRemove, kIdUp, kIdDown, kIdSeparator, kIdReset}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, midButtonX, y, buttonWidth, buttonHeight, TRUE);
            y += stackStep;
        }
    }
    int right = width - gap - Scale(90);
    const int buttonY = height - buttonHeight - gap;
    for (const int id : {kIdOk, kIdCancel}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, right, buttonY, Scale(90), buttonHeight, TRUE);
            right -= Scale(90) + Scale(6);
        }
    }
}

bool ToolbarDialog::Show(HWND owner, const core::CommandRegistry& registry,
                         const core::ToolbarItems& current, ToolbarPicks& out) {
    owner_ = owner;
    registry_ = &registry;
    current_ = current;

    // 候选：注册表里所有不是"隐藏"的命令，按类别分组、组内按注册顺序。
    // 已经在工具栏上的不再列（要拿下来去右边点"移除"就行）
    for (const core::CmdCategory category : kOrder) {
        for (const core::CmdId id : registry.ByCategory(category)) {
            const core::Command* command = registry.Find(id);
            if (command == nullptr || command->key.empty()) {
                continue;
            }
            if ((command->flags & core::kCmdHidden) != 0) {
                continue;   // 不出现的命令（只给快捷键/插件用）也不该出现在这里
            }
            if (std::find(current_.begin(), current_.end(), command->key) != current_.end()) {
                continue;
            }
            available_.push_back(command->key);
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ToolbarDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(720);
    const int height = Scale(430);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    const std::wstring title = core::Tr(L"dialog.toolbar.title", L"自定义工具栏");
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

    Make(L"STATIC", core::Tr(L"dialog.toolbar.available", L"可用命令（双击加进工具栏）").c_str(),
         WS_VISIBLE | SS_LEFT, kIdAvailableLabel);
    availableBox_ = Make(WC_LISTBOXW, L"",
                         WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY |
                             LBS_NOINTEGRALHEIGHT,
                         kIdAvailable);
    Make(L"STATIC", core::Tr(L"dialog.toolbar.current", L"工具栏上的按钮（顺序就是显示顺序）").c_str(),
         WS_VISIBLE | SS_LEFT, kIdCurrentLabel);
    currentBox_ = Make(WC_LISTBOXW, L"",
                       WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY |
                           LBS_NOINTEGRALHEIGHT,
                       kIdCurrent);
    Make(L"BUTTON", core::Tr(L"dialog.toolbar.add", L"添加 →").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdAdd);
    Make(L"BUTTON", core::Tr(L"dialog.toolbar.remove", L"← 移除").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdRemove);
    Make(L"BUTTON", core::Tr(L"dialog.toolbar.up", L"上移").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdUp);
    Make(L"BUTTON", core::Tr(L"dialog.toolbar.down", L"下移").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdDown);
    Make(L"BUTTON", core::Tr(L"dialog.toolbar.separatorButton", L"插入分隔符").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdSeparator);
    Make(L"BUTTON", core::Tr(L"dialog.toolbar.reset", L"恢复默认").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdReset);
    Make(L"BUTTON", core::Tr(L"dialog.toolbar.ok", L"确定").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdOk);
    Make(L"BUTTON", core::Tr(L"prefs.cancel", L"取消").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    FillLists();
    Layout();

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    Layout();
    if (currentBox_ != nullptr) {
        ::SetFocus(currentBox_);
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
    availableBox_ = nullptr;
    currentBox_ = nullptr;

    if (!accepted_) {
        return false;
    }
    out.custom = !useDefault_;
    if (useDefault_) {
        out.items = core::DefaultToolbarItems();
    } else {
        out.items = current_;
    }
    return true;
}

}  // namespace

bool CustomizeToolbar(HWND owner, const core::CommandRegistry& registry,
                      const core::ToolbarItems& current, ToolbarPicks& out) {
    ToolbarDialog dialog;
    return dialog.Show(owner, registry, current, out);
}

}  // namespace npcc::shell
