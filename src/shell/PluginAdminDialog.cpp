#include "shell/PluginAdminDialog.h"

#include <commctrl.h>

#include <map>
#include <string>
#include <vector>

#include "infra/Log.h"
#include "plugin/PluginHost.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccPluginAdmin";

enum : int {
    kListLabel = 1400,
    kList = 1401,
    kDetail = 1402,
    kToggle = 1403,
    kHint = 1404,
    kOk = 1410,
    kCancel = 1411,
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

class PluginAdmin {
public:
    bool Show(HWND owner, const npcc::plugin::PluginHost& plugins);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);
    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    void FillList();
    void RefreshDetail();
    std::wstring StatusText(std::size_t index) const;

    HWND                              owner_ = nullptr;
    HWND                              hwnd_ = nullptr;
    HFONT                             font_ = nullptr;
    const npcc::plugin::PluginHost*   plugins_ = nullptr;
    // 用户改过的启用状态：名字 → 是否禁用（只写"改过的"，没改的不动）
    std::map<std::wstring, bool>      pending_;
    bool                              done_ = false;
    bool                              accepted_ = false;
};

int PluginAdmin::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND PluginAdmin::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

std::wstring PluginAdmin::StatusText(std::size_t index) const {
    const auto& list = plugins_->Plugins();
    if (index >= list.size()) {
        return {};
    }
    const npcc::plugin::PluginStatus& status = list[index];
    const bool disabled = pending_.count(status.name) != 0 ? pending_.at(status.name)
                                                           : status.disabled;
    std::wstring text = status.name;
    if (!status.version.empty()) {
        text += L"  " + status.version;
    }
    if (disabled) {
        text += L"   [已禁用]";
    } else if (status.loaded) {
        text += L"   [已加载]";
    } else {
        text += L"   [未加载] " + status.reason;
    }
    return text;
}

void PluginAdmin::FillList() {
    HWND list = ::GetDlgItem(hwnd_, kList);
    if (list == nullptr) {
        return;
    }
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (std::size_t index = 0; index < plugins_->Plugins().size(); ++index) {
        const std::wstring text = StatusText(index);
        ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (!plugins_->Plugins().empty()) {
        ::SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
    RefreshDetail();
}

void PluginAdmin::RefreshDetail() {
    HWND list = ::GetDlgItem(hwnd_, kList);
    HWND detail = ::GetDlgItem(hwnd_, kDetail);
    HWND toggle = ::GetDlgItem(hwnd_, kToggle);
    if (list == nullptr || detail == nullptr) {
        return;
    }
    const LRESULT selected = ::SendMessageW(list, LB_GETCURSEL, 0, 0);
    const auto& all = plugins_->Plugins();
    if (selected < 0 || static_cast<std::size_t>(selected) >= all.size()) {
        ::SetWindowTextW(detail, L"（没有发现插件）");
        if (toggle != nullptr) {
            ::EnableWindow(toggle, FALSE);
        }
        return;
    }
    const npcc::plugin::PluginStatus& status = all[static_cast<std::size_t>(selected)];
    const bool disabled = pending_.count(status.name) != 0 ? pending_.at(status.name)
                                                          : status.disabled;
    std::wstring text = L"目录：" + status.directory.wstring() + L"\r\n";
    text += L"DLL：" + status.dllPath.wstring() + L"\r\n";
    if (!status.description.empty()) {
        text += L"说明：" + status.description + L"\r\n";
    }
    if (!status.author.empty()) {
        text += L"作者：" + status.author + L"\r\n";
    }
    if (!status.loaded && !disabled) {
        text += L"未加载原因：" + status.reason;
    }
    ::SetWindowTextW(detail, text.c_str());
    if (toggle != nullptr) {
        ::EnableWindow(toggle, TRUE);
        ::SendMessageW(toggle, BM_SETCHECK, disabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void PluginAdmin::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(10);
    const int row = Scale(24);
    const int buttonHeight = Scale(30);
    const int buttonWidth = Scale(110);
    const int width = client.right - client.left;
    int y = gap;
    if (HWND label = ::GetDlgItem(hwnd_, kListLabel); label != nullptr) {
        ::MoveWindow(label, gap, y, width - gap * 2, row, TRUE);
    }
    y += row;
    const int buttonY = client.bottom - buttonHeight - gap;
    const int listHeight = std::max(Scale(80), (buttonY - gap * 3 - row - Scale(90)) / 2);
    if (HWND list = ::GetDlgItem(hwnd_, kList); list != nullptr) {
        ::MoveWindow(list, gap, y, width - gap * 2, listHeight, TRUE);
    }
    y += listHeight + gap;
    if (HWND detail = ::GetDlgItem(hwnd_, kDetail); detail != nullptr) {
        ::MoveWindow(detail, gap, y, width - gap * 2, Scale(76), TRUE);
    }
    y += Scale(76) + Scale(4);
    if (HWND toggle = ::GetDlgItem(hwnd_, kToggle); toggle != nullptr) {
        ::MoveWindow(toggle, gap, y, width - gap * 2, row, TRUE);
    }
    y += row;
    if (HWND hint = ::GetDlgItem(hwnd_, kHint); hint != nullptr) {
        ::MoveWindow(hint, gap, y, width - gap * 2, row, TRUE);
    }
    int right = width - gap - buttonWidth;
    for (const int id : {kOk, kCancel}) {
        if (HWND button = ::GetDlgItem(hwnd_, id); button != nullptr) {
            ::MoveWindow(button, right, buttonY, buttonWidth, buttonHeight, TRUE);
            right -= buttonWidth + Scale(6);
        }
    }
}

bool PluginAdmin::Show(HWND owner, const npcc::plugin::PluginHost& plugins) {
    owner_ = owner;
    plugins_ = &plugins;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PluginAdmin::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(560);
    const int height = Scale(440);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"插件管理器",
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

    Make(L"STATIC", L"发现到的插件（选中一个看详情）：", SS_LEFT, kListLabel);
    Make(WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY, kList);
    Make(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, kDetail);
    Make(L"BUTTON", L"禁用选中的插件", WS_TABSTOP | BS_AUTOCHECKBOX, kToggle);
    Make(L"STATIC", L"改动写入 plugins.xml，**重启程序后生效**（插件不能安全卸载）。",
         SS_LEFT, kHint);
    Make(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, kOk);
    Make(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, kCancel);
    (void)::SendMessageW(hwnd_, DM_SETDEFID, kOk, 0);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    FillList();
    Layout();

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    Layout();

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

    if (!accepted_ || pending_.empty()) {
        return false;
    }
    // 落盘：只写用户真的改过的那些
    bool changed = false;
    for (const auto& item : pending_) {
        if (item.second == npcc::plugin::PluginHost::IsDisabled(item.first)) {
            continue;
        }
        std::wstring error;
        if (!npcc::plugin::PluginHost::SetDisabled(item.first, item.second, &error)) {
            infra::LogWarn(L"plugin", L"插件状态保存失败: " + error);
            continue;
        }
        changed = true;
    }
    return changed;
}

LRESULT CALLBACK PluginAdmin::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PluginAdmin*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<PluginAdmin*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT PluginAdmin::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kList && HIWORD(wParam) == LBN_SELCHANGE) {
                RefreshDetail();
                return 0;
            }
            if (id == kToggle) {
                const LRESULT selected =
                    ::SendMessageW(::GetDlgItem(hwnd_, kList), LB_GETCURSEL, 0, 0);
                const auto& all = plugins_->Plugins();
                if (selected >= 0 && static_cast<std::size_t>(selected) < all.size()) {
                    const bool disabled =
                        ::SendMessageW(::GetDlgItem(hwnd_, kToggle), BM_GETCHECK, 0, 0) ==
                        BST_CHECKED;
                    pending_[all[static_cast<std::size_t>(selected)].name] = disabled;
                    FillList();
                    ::SendMessageW(::GetDlgItem(hwnd_, kList), LB_SETCURSEL, selected, 0);
                    RefreshDetail();
                }
                return 0;
            }
            if (id == kOk) {
                accepted_ = true;
                done_ = true;
                return 0;
            }
            if (id == kCancel) {
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

bool ShowPluginManager(HWND owner, const npcc::plugin::PluginHost& plugins) {
    PluginAdmin dialog;
    return dialog.Show(owner, plugins);
}

}  // namespace npcc::shell
