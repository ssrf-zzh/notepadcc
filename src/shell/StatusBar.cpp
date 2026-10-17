#include "shell/StatusBar.h"

#include <commctrl.h>

#include <algorithm>

#include "shell/ChromeStyle.h"

namespace npcc::shell {
namespace {

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

}  // namespace

StatusBar::~StatusBar() {
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

bool StatusBar::Create(HWND parent, int controlId) {
    hwnd_ = ::CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                              WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | SBARS_TOOLTIPS,
                              0, 0, 0, 0, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd_ == nullptr) {
        return false;
    }
    // 只接管绘制：文本仍然由控件自己存，SB_GETTEXTW 照样读得到
    ::SetWindowSubclass(hwnd_, &StatusBar::SubclassProc, 1,
                        reinterpret_cast<DWORD_PTR>(this));

    font_ = CreateChromeFont(DpiOf(hwnd_), 90, false);
    // 把字体也交给控件自己：它按这个字体算自己的高度（Height() 读的就是它算出来的），
    // 不设的话控件用默认字体，和自绘用的字体差半号
    if (font_ != nullptr) {
        ::SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    SetText(0, L"就绪");
    SetText(1, L"");
    SetText(2, L"");
    SetText(3, L"");
    return true;
}

void StatusBar::SetTheme(bool dark, bool eyeCare) {
    if (dark_ == dark && eyeCare_ == eyeCare) {
        return;
    }
    dark_ = dark;
    eyeCare_ = eyeCare;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void StatusBar::Resize(const RECT& client) const {
    if (hwnd_ == nullptr) {
        return;
    }
    ::SendMessageW(hwnd_, WM_SIZE, 0, 0);

    // 分区宽度按客户区宽度比例分配，避免固定像素在高 DPI 下错位
    const int width = client.right - client.left;
    // 第 4 段留给宏录制指示：它必须有自己的位置，否则会被"下一次 UpdateStatus"冲掉
    int parts[4] = {width * 42 / 100, width * 68 / 100, width * 87 / 100, -1};
    ::SendMessageW(hwnd_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
    SetText(3, recording_ ? L"\u25cf REC" : authorText_);
    if (!authorTooltip_.empty()) {
        ::SendMessageW(hwnd_, SB_SETTIPTEXTW, 3,
                       reinterpret_cast<LPARAM>(authorTooltip_.c_str()));
    }
}

int StatusBar::Height() const {
    if (hwnd_ == nullptr) {
        return 0;
    }
    // 高度不能取 WM_SIZE 的返回值——实测这个用法下它恒为 0（附录 B 记了这个坑），
    // 外壳按 0 算边距的结果是编辑区一直盖着状态栏。改成问窗口自己：这条 WM_SIZE
    // 已经让控件按自己的字体把高度算好、也挪到位了，量出来就是真高度。
    ::SendMessageW(hwnd_, WM_SIZE, 0, 0);
    RECT rect{};
    if (::GetWindowRect(hwnd_, &rect) != 0) {
        const int height = rect.bottom - rect.top;
        if (height > 0) {
            return height;
        }
    }
    // 兜底：窗口还没被摆到位时按字体自己算（和控件内部的算法同一个口径）
    if (font_ == nullptr) {
        return 0;
    }
    const HDC dc = ::GetDC(hwnd_);
    if (dc == nullptr) {
        return 0;
    }
    const HGDIOBJ previous = ::SelectObject(dc, font_);
    TEXTMETRICW metrics{};
    const int textHeight = (::GetTextMetricsW(dc, &metrics) != 0) ? metrics.tmHeight : 0;
    ::SelectObject(dc, previous);
    ::ReleaseDC(hwnd_, dc);
    return textHeight > 0 ? textHeight + ::MulDiv(8, DpiOf(hwnd_), 96) : 0;
}

void StatusBar::SetText(int part, const std::wstring& text) const {
    if (hwnd_ != nullptr) {
        ::SendMessageW(hwnd_, SB_SETTEXTW, static_cast<WPARAM>(part),
                       reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void StatusBar::SetPosition(int32_t line, int32_t column, int32_t selectedBytes,
                            bool overwrite) const {
    std::wstring text = L"行 " + std::to_wstring(line + 1) + L", 列 " + std::to_wstring(column + 1);
    if (selectedBytes > 0) {
        text += L"  |  选中 " + std::to_wstring(selectedBytes) + L" 字节";
    }
    text += overwrite ? L"  |  改写" : L"  |  插入";
    SetText(0, text);
}

void StatusBar::SetDocumentInfo(const std::wstring& encoding, const std::wstring& eol,
                                const std::wstring& language, bool modified) const {
    SetText(1, encoding + L"  |  " + eol);
    SetText(2, language + (modified ? L"  |  已修改" : L""));
}

void StatusBar::SetMessage(const std::wstring& text) const {
    SetText(0, text);
}

void StatusBar::SetMacroRecording(bool recording) {
    recording_ = recording;
    SetText(3, recording ? L"\u25cf REC" : authorText_);
}

void StatusBar::SetAuthor(const std::wstring& text, const std::wstring& tooltip) {
    authorText_ = text;
    authorTooltip_ = tooltip;
    if (!recording_) {
        SetText(3, authorText_);
    }
    if (hwnd_ != nullptr && !tooltip.empty()) {
        ::SendMessageW(hwnd_, SB_SETTIPTEXTW, 3,
                       reinterpret_cast<LPARAM>(tooltip.c_str()));
    }
}

LRESULT CALLBACK StatusBar::SubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR /*subclassId*/,
                                         DWORD_PTR refData) {
    auto* self = reinterpret_cast<StatusBar*>(refData);
    switch (message) {
        case WM_ERASEBKGND:
            return 1;   // 自绘一次刷完，免得闪
        case WM_PAINT: {
            if (self != nullptr) {
                PAINTSTRUCT ps{};
                const HDC dc = ::BeginPaint(hwnd, &ps);
                self->Paint(dc);
                ::EndPaint(hwnd, &ps);
                return 0;
            }
            break;
        }
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, &StatusBar::SubclassProc, 1);
            break;
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{};
                ::GetCursorPos(&point);
                ::ScreenToClient(hwnd, &point);
                int edges[4]{};
                const LRESULT parts = ::SendMessageW(
                    hwnd, SB_GETPARTS, 4, reinterpret_cast<LPARAM>(edges));
                if (parts >= 4 && point.x >= edges[2]) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

void StatusBar::Paint(HDC dc) const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const ChromeColours colours = ChromeColoursFor(dark_, eyeCare_);
    const int dpi = DpiOf(hwnd_);

    const HBRUSH background = ::CreateSolidBrush(colours.strip);
    ::FillRect(dc, &client, background);
    ::DeleteObject(background);

    // 顶边一条细线：与编辑区分界（按 DPI 缩放，200% 下也是"一条线"）
    const int hairline = ::MulDiv(1, dpi, 96);
    RECT topLine{0, 0, client.right, hairline};
    const HBRUSH topBrush = ::CreateSolidBrush(colours.line);
    ::FillRect(dc, &topLine, topBrush);
    ::DeleteObject(topBrush);

    const HGDIOBJ previousFont = (font_ != nullptr) ? ::SelectObject(dc, font_) : nullptr;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, colours.statusText);

    int edges[8] = {0};
    const int partCount = static_cast<int>(::SendMessageW(
        hwnd_, SB_GETPARTS, static_cast<WPARAM>(8), reinterpret_cast<LPARAM>(edges)));
    const int textPad = ::MulDiv(10, dpi, 96);
    const int lineInset = ::MulDiv(4, dpi, 96);
    int left = 0;
    for (int i = 0; i < partCount && i < 8; ++i) {
        int right = edges[i];
        if (right <= 0) {
            right = client.right;   // -1 = 一直到最后
        }
        right = std::min<int>(right, client.right);
        if (right <= left) {
            left = right;
            continue;
        }
        if (i > 0) {
            RECT divider{left, lineInset, left + hairline, client.bottom - lineInset};
            const HBRUSH brush = ::CreateSolidBrush(colours.line);
            ::FillRect(dc, &divider, brush);
            ::DeleteObject(brush);
        }
        const LRESULT length =
            ::SendMessageW(hwnd_, SB_GETTEXTLENGTHW, static_cast<WPARAM>(i), 0);
        if (length > 0) {
            std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
            ::SendMessageW(hwnd_, SB_GETTEXTW, static_cast<WPARAM>(i),
                           reinterpret_cast<LPARAM>(buffer.data()));
            RECT textRect{left + textPad, 0, right - textPad, client.bottom};
            if (textRect.right > textRect.left) {
                const UINT alignment = (i == 3) ? DT_RIGHT : DT_LEFT;
                ::DrawTextW(dc, buffer.c_str(), -1, &textRect,
                            DT_SINGLELINE | DT_VCENTER | alignment | DT_END_ELLIPSIS |
                            DT_NOPREFIX);
            }
        }
        left = right;
    }
    if (previousFont != nullptr) {
        ::SelectObject(dc, previousFont);
    }

    // 右下角的缩放角标（原来由系统画，自绘之后补上，样式跟着 chrome 走）
    if ((::GetWindowLongPtrW(hwnd_, GWL_STYLE) & SBARS_SIZEGRIP) != 0) {
        const int unit = ::MulDiv(3, dpi, 96);
        const int x = client.right - ::MulDiv(4, dpi, 96);
        const int y = client.bottom - ::MulDiv(4, dpi, 96);
        const HPEN pen = ::CreatePen(PS_SOLID, hairline, colours.textMuted);
        const HGDIOBJ oldPen = ::SelectObject(dc, pen);
        for (int step = 1; step <= 3; ++step) {
            const int offset = step * unit * 2;
            ::MoveToEx(dc, x - offset, y, nullptr);
            ::LineTo(dc, x, y - offset);
        }
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);
    }
}

}  // namespace npcc::shell
