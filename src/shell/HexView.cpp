#include "shell/HexView.h"

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM

#include <algorithm>
#include <cwchar>
#include <vector>

#include "edit/HexModel.h"
#include "infra/Log.h"
#include "shell/MacroDialog.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccHexView";
constexpr int kLinesPerWheelNotch = 3;

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

std::wstring ToWide(const std::string& bytes) {
    if (bytes.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(),
                          need);
    return out;
}

}  // namespace

void HexView::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &HexView::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // 自己画背景（浅色/深色两套）
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);
}

LRESULT CALLBACK HexView::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<HexView*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<HexView*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

bool HexView::Create(HWND parent, int controlId) {
    parent_ = parent;
    dpi_ = DpiOf(parent);
    hwnd_ = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10,
                              10, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }
    CreateFonts();
    SetDark(false);
    return true;
}

void HexView::CreateFonts() {
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    LOGFONTW logFont{};
    // 等宽是硬要求：一行里的十六进制与 ASCII 要对得上，比例字体一进来整列就歪了
    ::wcscpy_s(logFont.lfFaceName, L"Consolas");
    logFont.lfHeight = -::MulDiv(10, dpi_, 72);
    logFont.lfWeight = FW_NORMAL;
    logFont.lfCharSet = DEFAULT_CHARSET;
    font_ = ::CreateFontIndirectW(&logFont);
    if (font_ == nullptr) {
        font_ = static_cast<HFONT>(::GetStockObject(ANSI_FIXED_FONT));
    }
    HDC dc = ::GetDC(hwnd_);
    const HGDIOBJ previous = ::SelectObject(dc, font_);
    TEXTMETRICW metrics{};
    ::GetTextMetricsW(dc, &metrics);
    charWidth_ = metrics.tmAveCharWidth > 0 ? metrics.tmAveCharWidth : 8;
    charHeight_ = metrics.tmHeight > 0 ? metrics.tmHeight : 16;
    ::SelectObject(dc, previous);
    ::ReleaseDC(hwnd_, dc);
}

void HexView::SetDark(bool dark) {
    dark_ = dark;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

int HexView::OffsetWidth() const {
    return edit::HexOffsetWidthFor(size_);
}

int HexView::RowCount() const {
    return static_cast<int>((size_ + edit::kHexBytesPerRow - 1) / edit::kHexBytesPerRow);
}

int HexView::VisibleRowCount() const {
    if (hwnd_ == nullptr || charHeight_ <= 0) {
        return 1;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int rows = (client.bottom - client.top) / charHeight_;
    return rows > 0 ? rows : 1;
}

std::uint64_t HexView::TopOffset() const {
    return topRow_ * edit::kHexBytesPerRow;
}

int HexView::RowBytesAt(std::uint64_t offset) const {
    if (offset >= size_) {
        return 0;
    }
    const std::uint64_t rest = size_ - offset;
    return rest < edit::kHexBytesPerRow ? static_cast<int>(rest) : edit::kHexBytesPerRow;
}

const std::uint8_t* HexView::BytesAt(std::uint64_t offset) const {
    if (!file_.Valid() || offset >= size_) {
        return nullptr;
    }
    return reinterpret_cast<const std::uint8_t*>(file_.Data()) + offset;
}

bool HexView::OpenFile(const std::wstring& path, std::wstring* error) {
    CloseFile();
    if (!file_.Open(path)) {
        if (error != nullptr) {
            *error = file_.Error();
        }
        return false;
    }
    path_ = path;
    size_ = file_.Size();
    topRow_ = 0;
    caret_ = 0;
    UpdateScrollBar();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void HexView::CloseFile() {
    file_.Close();
    path_.clear();
    size_ = 0;
    topRow_ = 0;
    caret_ = 0;
    if (hwnd_ != nullptr) {
        UpdateScrollBar();
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void HexView::SetVisible(bool visible) const {
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
}

void HexView::Resize(const RECT& area) const {
    if (hwnd_ != nullptr) {
        ::MoveWindow(hwnd_, area.left, area.top, area.right - area.left,
                     area.bottom - area.top, TRUE);
    }
}

void HexView::Focus() const {
    if (hwnd_ != nullptr) {
        ::SetFocus(hwnd_);
    }
}

void HexView::UpdateScrollBar() const {
    if (hwnd_ == nullptr) {
        return;
    }
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = RowCount() > 0 ? RowCount() - 1 : 0;
    info.nPage = static_cast<UINT>(VisibleRowCount());
    info.nPos = static_cast<int>(topRow_);
    ::SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
}

void HexView::ScrollTo(std::uint64_t topRow) {
    const std::uint64_t maxTop =
        RowCount() > VisibleRowCount() ? static_cast<std::uint64_t>(RowCount() - VisibleRowCount())
                                       : 0;
    const std::uint64_t clamped = std::min(topRow, maxTop);
    if (clamped == topRow_) {
        return;
    }
    topRow_ = clamped;
    UpdateScrollBar();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void HexView::EnsureCaretVisible() {
    if (size_ == 0) {
        return;
    }
    const std::uint64_t row = caret_ / edit::kHexBytesPerRow;
    const std::uint64_t visible = static_cast<std::uint64_t>(VisibleRowCount());
    if (row < topRow_) {
        ScrollTo(row);
    } else if (row >= topRow_ + visible) {
        ScrollTo(row - visible + 1);
    }
}

void HexView::SetCaret(std::uint64_t offset, bool notify) {
    if (size_ == 0) {
        return;
    }
    const std::uint64_t clamped = std::min(offset, size_ - 1);
    if (clamped == caret_ && !notify) {
        return;
    }
    caret_ = clamped;
    EnsureCaretVisible();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    if (notify && offsetCallback_) {
        offsetCallback_(caret_);
    }
}

void HexView::GoTo(std::uint64_t offset) {
    if (size_ == 0) {
        return;
    }
    caret_ = std::min(offset, size_ - 1);
    // "跳到偏移"按十六进制查看器的惯例把目标滚到**屏幕第一行**，而不是"刚好露出来"——
    // 用户是冲着那一段来的。键盘与鼠标走的仍是 SetCaret（贴边滚动就够）
    ScrollTo(caret_ / edit::kHexBytesPerRow);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    if (offsetCallback_) {
        offsetCallback_(caret_);
    }
}

std::string HexView::VisibleText() const {
    if (size_ == 0) {
        return {};
    }
    const std::uint64_t top = TopOffset();
    const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());
    // 注意：data 要传**整块缓冲区的起点**、startOffset 传起点偏移。
    // 这里传过已经偏移过的指针，结果偏移被应用了两次——屏幕上标着 0x190 的地方显示的
    // 是 0x320 的字节，而且是界面回归脚本逐字节比对时抓出来的（见附录 B）
    return edit::HexBlockText(reinterpret_cast<const std::uint8_t*>(file_.Data()),
                              static_cast<std::size_t>(size_), top,
                              rows * edit::kHexBytesPerRow, OffsetWidth());
}

void HexView::Paint(HDC dc, const RECT& client) {
    const COLORREF background = dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255);
    const COLORREF textColour = dark_ ? RGB(220, 220, 220) : RGB(30, 30, 30);
    const COLORREF offsetColour = dark_ ? RGB(120, 160, 200) : RGB(120, 120, 120);
    const COLORREF caretColour = dark_ ? RGB(60, 95, 145) : RGB(200, 220, 255);

    const HBRUSH backgroundBrush = ::CreateSolidBrush(background);
    ::FillRect(dc, &client, backgroundBrush);
    ::DeleteObject(backgroundBrush);

    const HGDIOBJ previousFont = ::SelectObject(dc, font_);
    ::SetBkMode(dc, TRANSPARENT);

    if (size_ == 0) {
        ::SetTextColor(dc, offsetColour);
        const wchar_t* hint = L"（没有打开文件）";
        ::TextOutW(dc, charWidth_, charHeight_, hint, static_cast<int>(::wcslen(hint)));
        ::SelectObject(dc, previousFont);
        return;
    }

    const std::uint64_t top = TopOffset();
    const int width = OffsetWidth();
    const std::uint64_t caretRow = caret_ / edit::kHexBytesPerRow;
    const int caretIndex = static_cast<int>(caret_ % edit::kHexBytesPerRow);
    const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());

    for (std::uint64_t row = 0; row < rows; ++row) {
        const std::uint64_t offset = top + row * edit::kHexBytesPerRow;
        if (offset >= size_) {
            break;
        }
        const int rowBytes = RowBytesAt(offset);
        const int y = static_cast<int>(row) * charHeight_;

        // 光标那个字节：先铺底，再把十六进制与 ASCII 两处的字画在底上
        if (topRow_ + row == caretRow && caretIndex < rowBytes) {
            const int hexColumn = edit::kHexTextStart + caretIndex * 3 +
                                  (caretIndex >= edit::kHexHalf ? 1 : 0);
            const RECT hexRect{hexColumn * charWidth_, y, (hexColumn + 2) * charWidth_,
                               y + charHeight_};
            const int asciiColumn = edit::kHexAsciiStart + caretIndex;
            const RECT asciiRect{asciiColumn * charWidth_, y, (asciiColumn + 1) * charWidth_,
                                 y + charHeight_};
            const HBRUSH caretBrush = ::CreateSolidBrush(caretColour);
            ::FillRect(dc, &hexRect, caretBrush);
            ::FillRect(dc, &asciiRect, caretBrush);
            ::DeleteObject(caretBrush);
        }

        const std::string line =
            edit::HexRowText(BytesAt(offset), static_cast<std::size_t>(rowBytes), offset, width);
        const std::wstring wide = ToWide(line);
        const std::wstring offsetPart = wide.substr(0, static_cast<std::size_t>(width));
        ::SetTextColor(dc, offsetColour);
        ::TextOutW(dc, 0, y, offsetPart.c_str(), static_cast<int>(offsetPart.size()));
        ::SetTextColor(dc, textColour);
        const std::wstring body = wide.substr(static_cast<std::size_t>(width));
        ::TextOutW(dc, width * charWidth_, y, body.c_str(), static_cast<int>(body.size()));
    }
    ::SelectObject(dc, previousFont);
}

void HexView::HandleKey(WPARAM key) {
    if (size_ == 0) {
        return;
    }
    const std::uint64_t page =
        static_cast<std::uint64_t>(VisibleRowCount()) * edit::kHexBytesPerRow;
    switch (key) {
        case VK_UP:
            SetCaret(caret_ >= edit::kHexBytesPerRow ? caret_ - edit::kHexBytesPerRow : 0, true);
            return;
        case VK_DOWN:
            SetCaret(caret_ + edit::kHexBytesPerRow, true);
            return;
        case VK_LEFT:
            SetCaret(caret_ > 0 ? caret_ - 1 : 0, true);
            return;
        case VK_RIGHT:
            SetCaret(caret_ + 1, true);
            return;
        case VK_PRIOR:
            SetCaret(caret_ >= page ? caret_ - page : 0, true);
            return;
        case VK_NEXT:
            SetCaret(caret_ + page, true);
            return;
        case VK_HOME:
            SetCaret(0, true);
            return;
        case VK_END:
            SetCaret(size_ - 1, true);
            return;
        default:
            return;
    }
}

void HexView::HandleClick(LPARAM position) {
    if (size_ == 0) {
        return;
    }
    const int x = GET_X_LPARAM(position);
    const int y = GET_Y_LPARAM(position);
    if (charHeight_ <= 0 || charWidth_ <= 0 || x < 0 || y < 0) {
        return;
    }
    const int column = x / charWidth_;
    const std::uint64_t row = topRow_ + static_cast<std::uint64_t>(y / charHeight_);
    const std::uint64_t offset = row * edit::kHexBytesPerRow;
    if (offset >= size_) {
        return;
    }
    int index = 0;
    if (!edit::HexByteAtColumn(column, RowBytesAt(offset), &index)) {
        return;
    }
    SetCaret(offset + static_cast<std::uint64_t>(index), true);
}

void HexView::PromptForOffset() {
    std::wstring text;
    if (!AskHexOffset(parent_, text)) {
        return;
    }
    std::uint64_t offset = 0;
    if (!edit::ParseHexJump(text, offset)) {
        ::MessageBoxW(parent_,
                      L"看不懂这个偏移。写十六进制请带 0x 前缀（例如 0x1F0），"
                      L"不带前缀的按十进制算。",
                      L"跳到偏移", MB_ICONINFORMATION);
        return;
    }
    GoTo(offset);
}

void HexView::PromptJump() {
    PromptForOffset();
}

LRESULT HexView::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd_, &paint);
            RECT client{};
            ::GetClientRect(hwnd_, &client);
            Paint(dc, client);
            ::EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;   // WM_PAINT 里自己铺背景，免得闪
        case WM_SIZE:
            UpdateScrollBar();
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_VSCROLL: {
            const int request = LOWORD(wParam);
            const int visible = VisibleRowCount();
            switch (request) {
                case SB_LINEUP:
                    ScrollTo(topRow_ > 0 ? topRow_ - 1 : 0);
                    return 0;
                case SB_LINEDOWN:
                    ScrollTo(topRow_ + 1);
                    return 0;
                case SB_PAGEUP:
                    ScrollTo(topRow_ > static_cast<std::uint64_t>(visible)
                                 ? topRow_ - static_cast<std::uint64_t>(visible)
                                 : 0);
                    return 0;
                case SB_PAGEDOWN:
                    ScrollTo(topRow_ + static_cast<std::uint64_t>(visible));
                    return 0;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:
                    ScrollTo(static_cast<std::uint64_t>(HIWORD(wParam)));
                    return 0;
                case SB_TOP:
                    ScrollTo(0);
                    return 0;
                case SB_BOTTOM:
                    ScrollTo(static_cast<std::uint64_t>(RowCount()));
                    return 0;
                default:
                    return 0;
            }
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int lines = delta > 0 ? -kLinesPerWheelNotch : kLinesPerWheelNotch;
            const std::int64_t target = static_cast<std::int64_t>(topRow_) + lines;
            ScrollTo(target > 0 ? static_cast<std::uint64_t>(target) : 0);
            return 0;
        }
        case WM_LBUTTONDOWN:
            ::SetFocus(hwnd_);
            dragging_ = true;
            HandleClick(lParam);
            return 0;
        case WM_MOUSEMOVE:
            if (dragging_ && (wParam & MK_LBUTTON) != 0) {
                HandleClick(lParam);
            }
            return 0;
        case WM_LBUTTONUP:
            dragging_ = false;
            return 0;
        case WM_KEYDOWN: {
            // Ctrl+G：与文本视图里"跳到指定行"同一个手势，在这里是"跳到偏移"
            if (wParam == L'G' && (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                PromptForOffset();
                return 0;
            }
            HandleKey(wParam);
            return 0;
        }
        case WM_GETTEXTLENGTH: {
            const std::wstring text = ToWide(VisibleText());
            return static_cast<LRESULT>(text.size());
        }
        case WM_GETTEXT: {
            // 系统消息（< WM_USER）会被 OS 封送，所以别的进程也能把这几行读回去。
            // 这是这个控件能被端到端验证的原因，也是"复制可见内容"的现成入口
            if (wParam == 0 || lParam == 0) {
                return 0;
            }
            const std::wstring text = ToWide(VisibleText());
            const std::size_t capacity = static_cast<std::size_t>(wParam);
            const std::size_t count =
                text.size() < capacity - 1 ? text.size() : capacity - 1;
            std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), text.c_str(), count);
            reinterpret_cast<wchar_t*>(lParam)[count] = L'\0';
            return static_cast<LRESULT>(count);
        }
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::shell
