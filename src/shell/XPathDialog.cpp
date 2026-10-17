#include "shell/XPathDialog.h"

#include <cwchar>

#include "core/XPath.h"
#include "infra/Log.h"
#include "shell/ChromeStyle.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccXPathQuery";

enum : int {
    kIdLabel   = 1500,
    kIdExpr    = 1501,
    kIdHint    = 1502,
    kIdRun     = 1510,
    kIdGoto    = 1511,
    kIdClose   = 1512,
    kIdResults = 1520,
    kIdStatus  = 1530,
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

std::wstring WindowText(HWND window) {
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int written = ::GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(written < 0 ? 0 : written));
    return text;
}

class XPathDialog {
public:
    bool Show(HWND owner, const std::string& utf8Text, XPathDialogResult& out);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void Layout();
    void RunQuery();
    void GotoSelected();
    void SetStatus(const std::wstring& text);
    std::wstring RowsText() const;

    HWND        owner_ = nullptr;
    HWND        hwnd_ = nullptr;
    HFONT       font_ = nullptr;
    bool        done_ = false;
    std::string text_;       // 文档全文（UTF-8），打开对话框时取一次
    std::vector<core::XPathMatch> matches_;
    std::vector<std::wstring> rows_;  // 与列表同内容；WM_GETTEXT 读回的就是它们
    XPathDialogResult result_;
};

int XPathDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND XPathDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void XPathDialog::SetStatus(const std::wstring& text) {
    if (HWND status = ::GetDlgItem(hwnd_, kIdStatus); status != nullptr) {
        ::SetWindowTextW(status, text.c_str());
    }
}

// 结果行按自绘面板同款规矩渲染成一段文本：界面回归脚本跨进程发 WM_GETTEXT
// 读回核对的就是它——标准 ListBox 的 LB_GETTEXT 要跨进程传指针，不可靠，
// 索性让对话框自己把"此刻列着什么"回出来（与结构树面板同一条路子）。
std::wstring XPathDialog::RowsText() const {
    std::wstring out;
    for (const std::wstring& row : rows_) {
        if (!out.empty()) {
            out += L"\r\n";
        }
        out += row;
    }
    return out;
}

void XPathDialog::RunQuery() {
    HWND expr = ::GetDlgItem(hwnd_, kIdExpr);
    HWND results = ::GetDlgItem(hwnd_, kIdResults);
    if (expr == nullptr || results == nullptr) {
        return;
    }
    const std::wstring expression = WindowText(expr);
    matches_.clear();
    rows_.clear();
    ::SendMessageW(results, LB_RESETCONTENT, 0, 0);

    const core::XPathResult query = core::XPathQuery(text_, expression);
    if (!query.ok) {
        SetStatus(L"错误：" + query.error);
        infra::LogInfo(L"tools", L"XPath 查询 '" + expression + L"' 失败：" + query.error);
        if (HWND go = ::GetDlgItem(hwnd_, kIdGoto); go != nullptr) {
            ::EnableWindow(go, FALSE);
        }
        return;
    }

    for (std::size_t index = 0; index < query.matches.size(); ++index) {
        const core::XPathMatch& match = query.matches[index];
        const std::wstring row = L"第 " + std::to_wstring(match.line) + L" 行  " +
                                 match.preview;
        rows_.push_back(row);
        const LRESULT added = ::SendMessageW(results, LB_ADDSTRING, 0,
                                             reinterpret_cast<LPARAM>(row.c_str()));
        if (added >= 0) {
            ::SendMessageW(results, LB_SETITEMDATA, static_cast<WPARAM>(added),
                           static_cast<LPARAM>(index));
        }
    }
    matches_ = query.matches;
    const int count = static_cast<int>(matches_.size());
    if (count > 0) {
        ::SendMessageW(results, LB_SETCURSEL, 0, 0);
    }
    if (HWND go = ::GetDlgItem(hwnd_, kIdGoto); go != nullptr) {
        ::EnableWindow(go, count > 0 ? TRUE : FALSE);
    }
    std::wstring status = L"共 " + std::to_wstring(count) + L" 个匹配";
    if (query.truncated) {
        status += L"（命中太多，只列前 " + std::to_wstring(core::kMaxXPathMatches) +
                  L" 条）";
    }
    if (query.treeTruncated) {
        status += L"；文档太大，结构树只建到前一部分，结果可能不完整";
    }
    SetStatus(status);
    infra::LogInfo(L"tools", L"XPath 查询 '" + expression + L"'：" + std::to_wstring(count) +
                                L" 个匹配");
}

void XPathDialog::GotoSelected() {
    HWND results = ::GetDlgItem(hwnd_, kIdResults);
    if (results == nullptr) {
        return;
    }
    const LRESULT selection = ::SendMessageW(results, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR || static_cast<std::size_t>(selection) >= matches_.size()) {
        return;
    }
    result_.accepted = true;
    result_.line = matches_[static_cast<std::size_t>(selection)].line;
    done_ = true;
}

LRESULT CALLBACK XPathDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                           LPARAM lParam) {
    auto* self = reinterpret_cast<XPathDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<XPathDialog*>(create->lpCreateParams);
        // 创建期间（WM_NCCREATE / WM_CREATE / 子控件创建）就用得上 hwnd_：
        // 兜底的 DefWindowProcW 要对同一个窗口说话，否则 WM_NCCREATE 被当成
        // 失败处理，整个窗口根本建不出来（RunCommandDialog 同款写法）。
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT XPathDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kIdRun:
                    RunQuery();
                    return 0;
                case kIdGoto:
                    GotoSelected();
                    return 0;
                case kIdClose:
                    done_ = true;
                    return 0;
                case kIdResults:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        GotoSelected();
                        return 0;
                    }
                    break;
                default:
                    break;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;
                return 0;
            }
            break;
        case WM_GETTEXTLENGTH:
            return static_cast<LRESULT>(RowsText().size());
        case WM_GETTEXT: {
            const std::wstring all = RowsText();
            const std::size_t capacity = static_cast<std::size_t>(wParam);
            auto* buffer = reinterpret_cast<wchar_t*>(lParam);
            if (buffer == nullptr || capacity == 0) {
                return 0;
            }
            std::size_t copy = all.size();
            if (copy > capacity - 1) {
                copy = capacity - 1;
            }
            std::wmemcpy(buffer, all.data(), copy);
            buffer[copy] = L'\0';
            return static_cast<LRESULT>(copy);
        }
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

void XPathDialog::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(10);
    const int row = Scale(18);
    const int editHeight = Scale(24);
    const int buttonHeight = Scale(28);
    const int buttonWidth = Scale(96);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int inner = width - gap * 2;

    int y = gap;
    if (HWND label = ::GetDlgItem(hwnd_, kIdLabel); label != nullptr) {
        ::MoveWindow(label, gap, y, inner, row, TRUE);
    }
    y += row + Scale(2);
    if (HWND hint = ::GetDlgItem(hwnd_, kIdHint); hint != nullptr) {
        ::MoveWindow(hint, gap, y, inner, row, TRUE);
    }
    y += row + Scale(6);

    if (HWND expr = ::GetDlgItem(hwnd_, kIdExpr); expr != nullptr) {
        ::MoveWindow(expr, gap, y, inner - buttonWidth - gap, editHeight, TRUE);
    }
    if (HWND run = ::GetDlgItem(hwnd_, kIdRun); run != nullptr) {
        ::MoveWindow(run, gap + inner - buttonWidth, y - Scale(2), buttonWidth,
                     buttonHeight, TRUE);
    }
    y += editHeight + gap;

    const int footer = buttonHeight + gap * 2 + row;  // 状态行 + 按钮行
    int resultsHeight = height - y - gap - footer;
    if (resultsHeight < Scale(80)) {
        resultsHeight = Scale(80);
    }
    if (HWND results = ::GetDlgItem(hwnd_, kIdResults); results != nullptr) {
        ::MoveWindow(results, gap, y, inner, resultsHeight, TRUE);
    }
    y += resultsHeight + gap;
    if (HWND status = ::GetDlgItem(hwnd_, kIdStatus); status != nullptr) {
        ::MoveWindow(status, gap, y, inner, row, TRUE);
    }
    y += row + gap;
    if (HWND go = ::GetDlgItem(hwnd_, kIdGoto); go != nullptr) {
        ::MoveWindow(go, gap + inner - buttonWidth * 2 - gap, y, buttonWidth, buttonHeight,
                     TRUE);
    }
    if (HWND close = ::GetDlgItem(hwnd_, kIdClose); close != nullptr) {
        ::MoveWindow(close, gap + inner - buttonWidth, y, buttonWidth, buttonHeight, TRUE);
    }
}

bool XPathDialog::Show(HWND owner, const std::string& utf8Text, XPathDialogResult& out) {
    owner_ = owner;
    text_ = utf8Text;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &XPathDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(720);
    const int height = Scale(460);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"XPath 查询",
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

    Make(L"STATIC",
         L"XPath 表达式（支持 /a/b、//a、*、@属性、text()；谓词 [序号]、[@属性]、"
         L"[子元素] 及其 = / != 比较）：",
         WS_VISIBLE | SS_LEFT, kIdLabel);
    Make(L"STATIC",
         L"查询以整篇文档为上下文；结果里双击一行跳过去。例：/catalog/book[@id='b1']",
         WS_VISIBLE | SS_LEFT, kIdHint);
    Make(L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, kIdExpr);
    Make(L"BUTTON", L"查询(&Q)", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdRun);
    Make(L"LISTBOX", L"",
         WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER | LBS_NOTIFY, kIdResults);
    Make(L"STATIC", L"输入表达式后按“查询”。", WS_VISIBLE | SS_LEFT, kIdStatus);
    Make(L"BUTTON", L"跳转(&J)", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdGoto);
    Make(L"BUTTON", L"关闭", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdClose);
    if (HWND go = ::GetDlgItem(hwnd_, kIdGoto); go != nullptr) {
        ::EnableWindow(go, FALSE);  // 还没有结果可跳
    }

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
    if (HWND expr = ::GetDlgItem(hwnd_, kIdExpr); expr != nullptr) {
        ::SetFocus(expr);
    }

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        // IsDialogMessage 让 Tab/回车按系统对话框语义走（回车 = 默认按钮 = 查询）
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
    if (result_.accepted) {
        out = result_;
    }
    return result_.accepted;
}

}  // namespace

bool ShowXPathDialog(HWND owner, const std::string& utf8Text, XPathDialogResult& out) {
    XPathDialog dialog;
    return dialog.Show(owner, utf8Text, out);
}

}  // namespace npcc::shell
