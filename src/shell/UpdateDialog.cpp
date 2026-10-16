#include "shell/UpdateDialog.h"

#include <commctrl.h>
#include <shellapi.h>

#include "core/Localization.h"
#include "core/Messages.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccUpdateCheck";

enum : int {
    kIdStatus = 1701,
    kIdNotes  = 1702,
    kIdOpen   = 1710,
    kIdClose  = 1711,
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

class UpdateDialogImpl {
public:
    void ShowChecking(HWND owner, infra::ThreadPool& pool,
                      std::shared_ptr<std::atomic<bool>> busy, std::string sourceUrl,
                      std::string currentVersion);
    void ShowAvailable(HWND owner, const infra::UpdateCheckReport& report);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void EnsureWindowClass();
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    void ApplyReport(const infra::UpdateCheckReport& report);

    HWND         owner_ = nullptr;
    HWND         hwnd_ = nullptr;
    HWND         status_ = nullptr;
    HWND         notes_ = nullptr;
    HWND         openButton_ = nullptr;
    HFONT        font_ = nullptr;
    bool         done_ = false;
    std::wstring availableDir_;
};

void UpdateDialogImpl::EnsureWindowClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &UpdateDialogImpl::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);
    registered = true;   // 重复注册同名类会失败，这里只认第一次（与 ToolbarDialog 同）
}

int UpdateDialogImpl::Scale(const int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND UpdateDialogImpl::Make(const wchar_t* cls, const wchar_t* text, const DWORD style,
                            const int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void UpdateDialogImpl::Layout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int margin = Scale(14);
    const int buttonWidth = Scale(120);
    const int buttonHeight = Scale(26);
    const int buttonY = client.bottom - margin - buttonHeight;
    const int statusWidth = client.right - margin * 2;

    ::MoveWindow(status_, margin, margin, statusWidth, Scale(52), TRUE);
    ::MoveWindow(notes_, margin, margin + Scale(56), statusWidth,
                 buttonY - margin - Scale(64), TRUE);
    ::MoveWindow(openButton_, client.right - margin * 2 - buttonWidth * 2 - Scale(8), buttonY,
                 buttonWidth, buttonHeight, TRUE);
    ::MoveWindow(::GetDlgItem(hwnd_, kIdClose), client.right - margin - buttonWidth, buttonY,
                 buttonWidth, buttonHeight, TRUE);
}

void UpdateDialogImpl::ApplyReport(const infra::UpdateCheckReport& report) {
    using Status = infra::UpdateCheckReport::Status;
    std::wstring statusText;
    std::wstring notesText;
    switch (report.status) {
        case Status::UpToDate:
            statusText = report.summary.empty() ? L"已是最新" : report.summary;
            break;
        case Status::Available:
            statusText = L"发现新版本 " + infra::WideFromUtf8(report.version) +
                         L"：已下载并通过 SHA-256 与清单签名校验，解压到 " +
                         report.packageDir + L"。程序不会自动覆盖正在运行的自己，请手动替换。";
            if (!report.notes.empty()) {
                notesText = L"更新说明：" + infra::WideFromUtf8(report.notes);
            }
            availableDir_ = report.packageDir;
            break;
        case Status::Error:
            statusText = L"检查失败：" + (report.detail.empty() ? report.summary : report.detail);
            break;
    }
    ::SetWindowTextW(status_, statusText.c_str());
    ::SetWindowTextW(notes_, notesText.c_str());
    if (!availableDir_.empty()) {
        ::EnableWindow(openButton_, TRUE);
    }
}

LRESULT CALLBACK UpdateDialogImpl::WndProcThunk(const HWND hwnd, const UINT message,
                                                const WPARAM wParam, const LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<UpdateDialogImpl*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<UpdateDialogImpl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT UpdateDialogImpl::WndProc(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
        case core::kMsgUpdateResult: {
            // 指针所有权在这里接过来（与函数列表/跨文件搜索同一约定）
            std::unique_ptr<infra::UpdateCheckReport> report(
                reinterpret_cast<infra::UpdateCheckReport*>(lParam));
            if (report != nullptr) {
                ApplyReport(*report);
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kIdClose || id == IDCANCEL) {
                done_ = true;
                return 0;
            }
            if (id == kIdOpen) {
                if (!availableDir_.empty()) {
                    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", availableDir_.c_str(),
                                    nullptr, SW_SHOWNORMAL);
                }
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
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Scale(520);
            info->ptMinTrackSize.y = Scale(220);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

void UpdateDialogImpl::ShowChecking(HWND owner, infra::ThreadPool& pool,
                                    std::shared_ptr<std::atomic<bool>> busy,
                                    std::string sourceUrl, std::string currentVersion) {
    EnsureWindowClass();
    owner_ = owner;
    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(620);
    const int height = Scale(240);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    const std::wstring title = core::Tr(L"dialog.update.title", L"检查更新");
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, title.c_str(),
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner,
                              nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        if (busy) {
            busy->store(false);
        }
        return;
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

    status_ = Make(L"STATIC", L"正在检查更新…", WS_VISIBLE | SS_LEFT, kIdStatus);
    notes_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdNotes);
    openButton_ = Make(L"BUTTON", core::Tr(L"dialog.update.open", L"打开下载目录").c_str(),
                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdOpen);
    ::EnableWindow(openButton_, FALSE);   // 有结果（且是新版本）才让点
    Make(L"BUTTON", core::Tr(L"dialog.update.close", L"关闭").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdClose);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](const HWND child, const LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    Layout();

    // 后台线程跑完整条校验链；结果投回**本对话框**。用户提前关掉对话框时
    // PostMessage 失败，报告随 unique_ptr 一起释放（与函数列表同一写法）
    const HWND target = hwnd_;
    infra::UpdateCheckOptions options;
    options.sourceUrl = std::move(sourceUrl);
    options.currentVersion = std::move(currentVersion);
    std::wstring sourceForDisplay = infra::WideFromUtf8(options.sourceUrl);
    ::SetWindowTextW(status_, (L"正在检查更新： " + sourceForDisplay).c_str());

    pool.Submit([target, busy, options]() mutable {
        auto report = std::make_unique<infra::UpdateCheckReport>(infra::RunUpdateCheck(options));
        if (busy) {
            busy->store(false);
        }
        if (::PostMessageW(target, core::kMsgUpdateResult, 0,
                           reinterpret_cast<LPARAM>(report.get())) == 0) {
            return;   // 对话框没了：指针随 unique_ptr 释放
        }
        report.release();
    });

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    Layout();
    if (openButton_ != nullptr) {
        ::SetFocus(openButton_);
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
    status_ = nullptr;
    notes_ = nullptr;
    openButton_ = nullptr;
}

void UpdateDialogImpl::ShowAvailable(HWND owner, const infra::UpdateCheckReport& report) {
    EnsureWindowClass();
    // 结构与 ShowChecking 相同，只是不提交任务、直接把结果摆上去
    owner_ = owner;
    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(620);
    const int height = Scale(240);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    const std::wstring title = core::Tr(L"dialog.update.title", L"检查更新");
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, title.c_str(),
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner,
                              nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return;
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
    status_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdStatus);
    notes_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdNotes);
    openButton_ = Make(L"BUTTON", core::Tr(L"dialog.update.open", L"打开下载目录").c_str(),
                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdOpen);
    ::EnableWindow(openButton_, FALSE);
    Make(L"BUTTON", core::Tr(L"dialog.update.close", L"关闭").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdClose);
    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](const HWND child, const LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    ApplyReport(report);
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
    status_ = nullptr;
    notes_ = nullptr;
    openButton_ = nullptr;
}

}  // namespace

void UpdateDialog::ShowChecking(HWND owner, infra::ThreadPool& pool,
                                const std::shared_ptr<std::atomic<bool>>& busy,
                                std::string sourceUrl, std::string currentVersion) {
    UpdateDialogImpl dialog;
    dialog.ShowChecking(owner, pool, busy, std::move(sourceUrl), std::move(currentVersion));
}

void UpdateDialog::ShowAvailable(HWND owner, const infra::UpdateCheckReport& report) {
    UpdateDialogImpl dialog;
    dialog.ShowAvailable(owner, report);
}

}  // namespace npcc::shell
