#include "shell/ColumnDialog.h"

#include <commctrl.h>

#include <algorithm>
#include <string>

#include "infra/Log.h"

namespace npcc::shell {
namespace {

constexpr int kGap = 8;
constexpr int kRowHeight = 24;
constexpr int kPreviewRows = 8;
constexpr const wchar_t* kWindowClass = L"notepadccColumnDialog";

class ColumnDialog {
public:
    bool Show(HWND owner, int lineCount, ColumnDialogResult& result);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int   Scale(int value) const;
    void  Layout();
    HWND  Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);
    void  RefreshPreview();
    void  ReadFields();
    std::wstring TextOf(int id) const;
    int   IntOf(int id, int fallback) const;
    long long LongOf(int id, long long fallback) const;

    HWND                 hwnd_ = nullptr;
    HWND                 owner_ = nullptr;
    HFONT                font_ = nullptr;
    int                  lineCount_ = 0;
    bool                 done_ = false;
    ColumnDialogResult*  result_ = nullptr;
};

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
    return ::MulDiv(1, static_cast<int>(dpi), 96) > 0 ? static_cast<int>(dpi) : 96;
}

}  // namespace

int ColumnDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND ColumnDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                        DWORD exStyle) {
    return ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

bool ColumnDialog::Show(HWND owner, int lineCount, ColumnDialogResult& result) {
    owner_ = owner;
    lineCount_ = std::max(1, lineCount);
    result_ = &result;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ColumnDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(460);
    const int height = Scale(360);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"列块编辑",
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

    Make(L"STATIC", L"插入内容（\\i\\ 表示序号）：", WS_VISIBLE, -1);
    Make(L"EDIT", L"\\i\\", WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
         kColumnTemplateEdit);
    Make(L"STATIC", L"起始值：", WS_VISIBLE, -1);
    Make(L"EDIT", L"1", WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
         kColumnStartEdit);
    Make(L"STATIC", L"步长：", WS_VISIBLE, -1);
    Make(L"EDIT", L"1", WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
         kColumnStepEdit);
    Make(L"STATIC", L"补零位数：", WS_VISIBLE, -1);
    Make(L"EDIT", L"0", WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
         kColumnPadEdit);
    Make(L"STATIC", L"进制：", WS_VISIBLE, -1);
    HWND baseCombo = Make(WC_COMBOBOXW, L"",
                          WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                          kColumnBaseCombo);
    if (baseCombo != nullptr) {
        ::SendMessageW(baseCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"十进制"));
        ::SendMessageW(baseCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"十六进制"));
        ::SendMessageW(baseCombo, CB_SETCURSEL, 0, 0);
    }
    Make(L"STATIC", L"每 N 行重置（0=不重置）：", WS_VISIBLE, -1);
    Make(L"EDIT", L"0", WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
         kColumnResetEdit);

    Make(L"STATIC", L"预览：", WS_VISIBLE, -1);
    Make(L"LISTBOX", L"", WS_VISIBLE | WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
         kColumnPreviewList);

    Make(L"BUTTON", L"插入", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kColumnInsertButton);
    Make(L"BUTTON", L"删除列块内容", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
         kColumnDeleteButton);
    Make(L"BUTTON", L"取消", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kColumnCancelButton);

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
    RefreshPreview();

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetFocus(::GetDlgItem(hwnd_, kColumnTemplateEdit));

    // 局部模态循环：这个对话框很小，用独立循环比改造主循环简单得多
    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        // 变量名不要叫 result：会遮蔽 Show() 的参数（MSVC /W4 的 C4457）
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
    return result_->apply || result_->deleteColumn;
}

int ColumnDialog::IntOf(int id, int fallback) const {
    const std::wstring text = TextOf(id);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

long long ColumnDialog::LongOf(int id, long long fallback) const {
    const std::wstring text = TextOf(id);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoll(text);
    } catch (...) {
        return fallback;
    }
}

std::wstring ColumnDialog::TextOf(int id) const {
    const HWND control = ::GetDlgItem(hwnd_, id);
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

void ColumnDialog::ReadFields() {
    result_->options.templateText = TextOf(kColumnTemplateEdit);
    result_->options.start = LongOf(kColumnStartEdit, 1);
    result_->options.step = LongOf(kColumnStepEdit, 1);
    result_->options.zeroPad = IntOf(kColumnPadEdit, 0);
    result_->options.resetEvery = IntOf(kColumnResetEdit, 0);
    result_->options.lineCount = lineCount_;
    const LRESULT base = ::SendMessageW(::GetDlgItem(hwnd_, kColumnBaseCombo), CB_GETCURSEL, 0, 0);
    result_->options.base = (base == 1) ? 16 : 10;
}

void ColumnDialog::RefreshPreview() {
    ReadFields();
    HWND list = ::GetDlgItem(hwnd_, kColumnPreviewList);
    if (list == nullptr) {
        return;
    }
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);

    npcc::edit::ColumnInsertOptions previewOptions = result_->options;
    previewOptions.lineCount = std::min(lineCount_, kPreviewRows);
    const std::vector<std::wstring> texts = npcc::edit::BuildColumnInsertTexts(previewOptions);
    for (const std::wstring& text : texts) {
        ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (lineCount_ > kPreviewRows) {
        const std::wstring more = L"…（共 " + std::to_wstring(lineCount_) + L" 行）";
        ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(more.c_str()));
    }
}

void ColumnDialog::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(kGap);
    const int rowHeight = Scale(kRowHeight);
    const int labelWidth = Scale(150);
    const int width = client.right - client.left;

    int y = gap;

    // 6 个输入行 + 1 个"预览："标签，按创建顺序取出来重新摆放
    const int ids[] = {kColumnTemplateEdit, kColumnStartEdit, kColumnStepEdit,
                       kColumnPadEdit, kColumnBaseCombo, kColumnResetEdit};
    std::vector<HWND> statics;
    ::EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM param) -> BOOL {
            auto* out = reinterpret_cast<std::vector<HWND>*>(param);
            wchar_t cls[64] = {};
            ::GetClassNameW(child, cls, 64);
            if (::wcscmp(cls, L"Static") == 0) {
                out->push_back(child);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&statics));

    // 前 6 个 STATIC 分别是各行标签，最后一个是"预览："
    for (std::size_t i = 0; i < 6 && i < statics.size(); ++i) {
        ::MoveWindow(statics[i], gap, y + Scale(4), labelWidth - gap, rowHeight, TRUE);
        const HWND control = ::GetDlgItem(hwnd_, ids[i]);
        if (control != nullptr) {
            const int controlWidth = (i == 4) ? Scale(120) : width - labelWidth - gap * 2;
            ::MoveWindow(control, gap + labelWidth, y, controlWidth,
                         (i == 4) ? rowHeight * 6 : rowHeight, TRUE);
        }
        y += rowHeight + gap / 2;
    }

    y += gap / 2;
    const int previewLabelIndex = 6;
    if (statics.size() > previewLabelIndex) {
        ::MoveWindow(statics[previewLabelIndex], gap, y + Scale(4), labelWidth - gap, rowHeight,
                     TRUE);
    }
    const int buttonHeight = Scale(30);
    const int previewHeight = client.bottom - y - buttonHeight - gap * 3;
    if (HWND list = ::GetDlgItem(hwnd_, kColumnPreviewList); list != nullptr) {
        ::MoveWindow(list, gap, y + rowHeight, width - gap * 2, std::max(Scale(40), previewHeight),
                     TRUE);
    }

    const int buttonY = client.bottom - buttonHeight - gap;
    const int buttonWidth = Scale(110);
    int x = width - gap - buttonWidth;
    for (const int id : {kColumnCancelButton, kColumnDeleteButton, kColumnInsertButton}) {
        if (HWND control = ::GetDlgItem(hwnd_, id); control != nullptr) {
            ::MoveWindow(control, x, buttonY, buttonWidth, buttonHeight, TRUE);
            x -= buttonWidth + gap / 2;
        }
    }
}

LRESULT CALLBACK ColumnDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                            LPARAM lParam) {
    ColumnDialog* self = reinterpret_cast<ColumnDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ColumnDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT ColumnDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notify = HIWORD(wParam);
            switch (id) {
                case kColumnInsertButton:
                    ReadFields();
                    result_->apply = true;
                    done_ = true;
                    return 0;
                case kColumnDeleteButton:
                    result_->deleteColumn = true;
                    done_ = true;
                    return 0;
                case kColumnCancelButton:
                    done_ = true;
                    return 0;
                case kColumnBaseCombo:
                    if (notify == CBN_SELCHANGE) {
                        RefreshPreview();
                    }
                    return 0;
                case kColumnTemplateEdit:
                case kColumnStartEdit:
                case kColumnStepEdit:
                case kColumnPadEdit:
                case kColumnResetEdit:
                    if (notify == EN_CHANGE) {
                        RefreshPreview();
                    }
                    return 0;
                default:
                    break;
            }
            break;
        }
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

bool ShowColumnDialog(HWND owner, int lineCount, ColumnDialogResult& result) {
    ColumnDialog dialog;
    return dialog.Show(owner, lineCount, result);
}

}  // namespace npcc::shell
