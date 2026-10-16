#include "shell/MarkdownPreview.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>

#include "infra/Log.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccMarkdownPreview";
constexpr std::size_t kMaxSourceBytes = 512 * 1024;   // 超过这个长度只渲染前一段

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

// UTF-8 的一个字符有多长（1~4 字节）。折行必须按字符切，按字节切会把汉字劈成两半
std::size_t Utf8Length(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;   // 非法前导字节：当一个字节处理，至少不越界
}

// 一段 UTF-8 文本占几个字符格（等宽字体下每个字符一格）
int Utf8Cells(std::string_view text) {
    int cells = 0;
    for (std::size_t index = 0; index < text.size();) {
        index += Utf8Length(static_cast<unsigned char>(text[index]));
        ++cells;
    }
    return cells;
}

}  // namespace

void MarkdownPreview::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MarkdownPreview::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);
}

LRESULT CALLBACK MarkdownPreview::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                               LPARAM lParam) {
    auto* self = reinterpret_cast<MarkdownPreview*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MarkdownPreview*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

bool MarkdownPreview::Create(HWND parent, int controlId) {
    dpi_ = DpiOf(parent);
    hwnd_ = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10,
                              10, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }
    CreateFonts();
    return true;
}

void MarkdownPreview::CreateFonts() {
    const auto make = [this](int weight, bool italic) {
        LOGFONTW logFont{};
        ::wcscpy_s(logFont.lfFaceName, L"Consolas");
        logFont.lfHeight = -::MulDiv(10, dpi_, 72);
        logFont.lfWeight = weight;
        logFont.lfItalic = italic ? TRUE : FALSE;
        logFont.lfCharSet = DEFAULT_CHARSET;
        return ::CreateFontIndirectW(&logFont);
    };
    if (font_ != nullptr) {
        ::DeleteObject(font_);
    }
    if (boldFont_ != nullptr) {
        ::DeleteObject(boldFont_);
    }
    if (italicFont_ != nullptr) {
        ::DeleteObject(italicFont_);
    }
    font_ = make(FW_NORMAL, false);
    boldFont_ = make(FW_BOLD, false);
    italicFont_ = make(FW_NORMAL, true);
    if (font_ == nullptr) {
        font_ = static_cast<HFONT>(::GetStockObject(ANSI_FIXED_FONT));
    }
    if (boldFont_ == nullptr) {
        boldFont_ = font_;
    }
    if (italicFont_ == nullptr) {
        italicFont_ = font_;
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

void MarkdownPreview::SetDark(bool dark) {
    dark_ = dark;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void MarkdownPreview::Clear() {
    source_.clear();
    notice_.clear();
    lines_.clear();
    topLine_ = 0;
    UpdateScrollBar();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void MarkdownPreview::SetMarkdown(std::string utf8) {
    source_ = std::move(utf8);
    notice_.clear();
    if (source_.size() > kMaxSourceBytes) {
        source_.resize(kMaxSourceBytes);
        // 截断要在字符边界上：截在 UTF-8 序列中间会渲染出一个乱码字符
        while (!source_.empty() &&
               (static_cast<unsigned char>(source_.back()) & 0xC0) == 0x80) {
            source_.pop_back();
        }
        notice_ = "（内容太长，只预览前 512 KB）";
    }
    Rebuild();
}

void MarkdownPreview::Rebuild() {
    lines_.clear();
    if (!notice_.empty()) {
        Line line;
        line.block = core::MarkdownBlockKind::Quote;
        line.segments.push_back(Segment{core::MarkdownSpanKind::Text, notice_});
        lines_.push_back(std::move(line));
    }
    const std::vector<core::MarkdownBlock> blocks = core::ParseMarkdown(source_);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const core::MarkdownBlock& block = blocks[index];
        // 块之间空一行（标题、分隔线前后多空一行），读起来才像排版过的文档
        const bool breathing = block.kind == core::MarkdownBlockKind::Heading ||
                               block.kind == core::MarkdownBlockKind::Rule ||
                               block.kind == core::MarkdownBlockKind::Code;
        if (!lines_.empty() && breathing) {
            lines_.emplace_back();
        }
        AppendBlockLines(block);
        if (block.kind == core::MarkdownBlockKind::Heading && !lines_.empty()) {
            lines_.emplace_back();
        }
    }
    topLine_ = 0;
    UpdateScrollBar();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void MarkdownPreview::AppendBlockLines(const core::MarkdownBlock& block) {
    switch (block.kind) {
        case core::MarkdownBlockKind::Code: {
            // 代码块按原样一行一行放（**不折行**：代码折了更难读）
            std::size_t start = 0;
            if (block.code.empty()) {
                lines_.emplace_back();
                Line& line = lines_.back();
                line.block = core::MarkdownBlockKind::Code;
                break;
            }
            while (start <= block.code.size()) {
                const std::size_t next = block.code.find('\n', start);
                Line line;
                line.block = core::MarkdownBlockKind::Code;
                line.segments.push_back(Segment{
                    core::MarkdownSpanKind::Text,
                    next == std::string::npos ? block.code.substr(start)
                                              : block.code.substr(start, next - start)});
                lines_.push_back(std::move(line));
                if (next == std::string::npos) {
                    break;
                }
                start = next + 1;
            }
            break;
        }
        case core::MarkdownBlockKind::Rule: {
            Line line;
            line.block = core::MarkdownBlockKind::Rule;
            line.rule = true;
            lines_.push_back(std::move(line));
            break;
        }
        case core::MarkdownBlockKind::List: {
            int indent = std::max(block.level, 0) * 2;
            std::string prefix(indent > 0 ? static_cast<std::size_t>(indent) : 0, ' ');
            prefix += block.ordered ? std::to_string(block.number) + ". " : "\xE2\x80\xA2 ";
            WrapInto(block.kind, block.level, block.spans, prefix);
            break;
        }
        case core::MarkdownBlockKind::Quote: {
            WrapInto(block.kind, block.level, block.spans, "\xE2\x94\x82 ");   // "│ "
            break;
        }
        case core::MarkdownBlockKind::Heading:
            WrapInto(block.kind, block.level, block.spans, "");
            break;
        default:
            WrapInto(block.kind, block.level, block.spans, "");
            break;
    }
}

void MarkdownPreview::WrapInto(core::MarkdownBlockKind block, int level,
                               const std::vector<core::MarkdownSpan>& spans,
                               const std::string& prefix) {
    int width = 80;   // 窗口还没成型时给一个保守宽度，免得算出 0 列
    if (hwnd_ != nullptr && charWidth_ > 0) {
        RECT client{};
        ::GetClientRect(hwnd_, &client);
        const int columns = (client.right - client.left) / charWidth_;
        if (columns >= 20) {
            width = columns;
        }
    }
    const int indentCells = Utf8Cells(prefix);
    const std::string lead(static_cast<std::size_t>(indentCells), ' ');
    const std::size_t startCount = lines_.size();

    Line current;
    current.block = block;
    current.level = level;
    int used = 0;
    if (!prefix.empty()) {
        current.segments.push_back(Segment{core::MarkdownSpanKind::Text, prefix});
        used = indentCells;
    }
    const auto flush_line = [&]() {
        lines_.push_back(std::move(current));
        current = Line{};
        current.block = block;
        current.level = level;
        used = 0;
        if (indentCells > 0) {
            // 续行缩进对齐到前缀：列表项折行后仍从文字处开始，不顶到行首
            current.segments.push_back(Segment{core::MarkdownSpanKind::Text, lead});
            used = indentCells;
        }
    };

    for (const core::MarkdownSpan& span : spans) {
        std::size_t index = 0;
        while (index < span.text.size()) {
            const int available = width - used;
            if (available <= 0) {
                flush_line();
                continue;
            }
            // 本行最多再放 available 个字符；中途如果碰到空格，就断在最后一个空格处
            std::size_t cursor = index;
            std::size_t lastSpace = std::string::npos;
            int cells = 0;
            while (cursor < span.text.size() && cells < available) {
                if (span.text[cursor] == ' ') {
                    lastSpace = cursor;
                }
                cursor += Utf8Length(static_cast<unsigned char>(span.text[cursor]));
                ++cells;
            }
            if (cursor < span.text.size() && lastSpace != std::string::npos &&
                lastSpace > index) {
                cursor = lastSpace;
            }
            const std::string piece = span.text.substr(index, cursor - index);
            if (!piece.empty()) {
                if (!current.segments.empty() &&
                    current.segments.back().kind == span.kind) {
                    current.segments.back().text += piece;   // 同类的片段合成一段
                } else {
                    current.segments.push_back(Segment{span.kind, piece});
                }
                used += Utf8Cells(piece);
            }
            index = cursor;
            if (index >= span.text.size()) {
                break;
            }
            if (span.text[index] == ' ') {
                ++index;   // 断点上的空格不进下一行
            }
            flush_line();
        }
    }
    // 本块一行都没产出（空标题、空列表项）时也要占一行，否则面板上会"少一块"
    if (!current.segments.empty() || lines_.size() == startCount) {
        lines_.push_back(std::move(current));
    }
}

int MarkdownPreview::VisibleLineCount() const {
    if (hwnd_ == nullptr || charHeight_ <= 0) {
        return 1;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int rows = (client.bottom - client.top) / charHeight_;
    return rows > 0 ? rows : 1;
}

void MarkdownPreview::UpdateScrollBar() const {
    if (hwnd_ == nullptr) {
        return;
    }
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = lines_.empty() ? 0 : static_cast<int>(lines_.size()) - 1;
    info.nPage = static_cast<UINT>(VisibleLineCount());
    info.nPos = topLine_;
    ::SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
}

void MarkdownPreview::ScrollTo(int topLine) {
    const int maxTop = std::max(0, static_cast<int>(lines_.size()) - VisibleLineCount());
    const int clamped = std::min(std::max(topLine, 0), maxTop);
    if (clamped == topLine_) {
        return;
    }
    topLine_ = clamped;
    UpdateScrollBar();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

std::string MarkdownPreview::LineText(const Line& line) const {
    std::string text;
    for (const Segment& segment : line.segments) {
        text += segment.text;
    }
    return text;
}

std::string MarkdownPreview::VisibleText() const {
    std::string text;
    const int first = std::max(topLine_, 0);
    const int last = std::min(static_cast<int>(lines_.size()), first + VisibleLineCount());
    for (int index = first; index < last; ++index) {
        if (!text.empty()) {
            text += "\r\n";
        }
        text += LineText(lines_[static_cast<std::size_t>(index)]);
    }
    return text;
}

void MarkdownPreview::Paint(HDC dc, const RECT& client) {
    const COLORREF background = dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255);
    const COLORREF textColour = dark_ ? RGB(220, 220, 220) : RGB(30, 30, 30);
    const COLORREF headingColour = dark_ ? RGB(86, 156, 214) : RGB(11, 74, 143);
    const COLORREF codeBack = dark_ ? RGB(42, 42, 42) : RGB(242, 242, 242);
    const COLORREF codeColour = dark_ ? RGB(212, 212, 212) : RGB(30, 30, 30);
    const COLORREF quoteColour = dark_ ? RGB(157, 165, 176) : RGB(87, 96, 106);
    const COLORREF linkColour = dark_ ? RGB(79, 193, 255) : RGB(9, 105, 218);
    const COLORREF ruleColour = dark_ ? RGB(90, 90, 90) : RGB(208, 215, 222);

    const HBRUSH backgroundBrush = ::CreateSolidBrush(background);
    ::FillRect(dc, &client, backgroundBrush);
    ::DeleteObject(backgroundBrush);
    ::SetBkMode(dc, TRANSPARENT);

    if (lines_.empty()) {
        const HGDIOBJ previous = ::SelectObject(dc, font_);
        ::SetTextColor(dc, quoteColour);
        const wchar_t* hint = L"（预览面板：打开或切到一份 Markdown 文档就会显示在这里）";
        ::TextOutW(dc, charWidth_, charHeight_, hint, static_cast<int>(::wcslen(hint)));
        ::SelectObject(dc, previous);
        return;
    }

    const std::uint32_t totalRows = static_cast<std::uint32_t>(VisibleLineCount());
    for (std::uint32_t row = 0; row < totalRows; ++row) {
        const std::size_t index = static_cast<std::size_t>(topLine_) + row;
        if (index >= lines_.size()) {
            break;
        }
        const Line& line = lines_[static_cast<std::size_t>(index)];
        const int y = static_cast<int>(row) * charHeight_;
        if (line.rule) {
            const HPEN pen = ::CreatePen(PS_SOLID, 1, ruleColour);
            const HGDIOBJ previousPen = ::SelectObject(dc, pen);
            const int lineY = y + charHeight_ / 2;
            ::MoveToEx(dc, 0, lineY, nullptr);
            ::LineTo(dc, client.right, lineY);
            ::SelectObject(dc, previousPen);
            ::DeleteObject(pen);
            continue;
        }
        if (line.block == core::MarkdownBlockKind::Code) {
            const RECT band{0, y, client.right, y + charHeight_};
            const HBRUSH brush = ::CreateSolidBrush(codeBack);
            ::FillRect(dc, &band, brush);
            ::DeleteObject(brush);
        }
        int x = 0;
        for (const Segment& segment : line.segments) {
            HFONT font = font_;
            COLORREF colour = textColour;
            switch (segment.kind) {
                case core::MarkdownSpanKind::Bold:
                    font = boldFont_;
                    break;
                case core::MarkdownSpanKind::Italic:
                    font = italicFont_;
                    break;
                case core::MarkdownSpanKind::Code:
                    font = font_;
                    colour = codeColour;
                    break;
                case core::MarkdownSpanKind::Link:
                    colour = linkColour;
                    break;
                default:
                    break;
            }
            if (line.block == core::MarkdownBlockKind::Heading) {
                font = boldFont_;
                colour = headingColour;
            } else if (line.block == core::MarkdownBlockKind::Code) {
                colour = codeColour;
            } else if (line.block == core::MarkdownBlockKind::Quote) {
                colour = segment.kind == core::MarkdownSpanKind::Link ? linkColour : quoteColour;
            }
            const HGDIOBJ previousFont = ::SelectObject(dc, font);
            ::SetTextColor(dc, colour);
            const std::wstring wide = ToWide(segment.text);
            ::TextOutW(dc, x, y, wide.c_str(), static_cast<int>(wide.size()));
            ::SelectObject(dc, previousFont);
            // 等宽字体下每个字符一格：位置按**字符数**推进（不是字节数）
            int cells = 0;
            for (std::size_t cursor = 0; cursor < segment.text.size();) {
                cursor += Utf8Length(static_cast<unsigned char>(segment.text[cursor]));
                ++cells;
            }
            x += cells * charWidth_;
        }
    }
}

LRESULT MarkdownPreview::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
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
            return 1;
        case WM_SIZE:
            Rebuild();   // 宽度变了要重新折行
            return 0;
        case WM_VSCROLL: {
            const int request = LOWORD(wParam);
            switch (request) {
                case SB_LINEUP:
                    ScrollTo(topLine_ - 1);
                    return 0;
                case SB_LINEDOWN:
                    ScrollTo(topLine_ + 1);
                    return 0;
                case SB_PAGEUP:
                    ScrollTo(topLine_ - VisibleLineCount());
                    return 0;
                case SB_PAGEDOWN:
                    ScrollTo(topLine_ + VisibleLineCount());
                    return 0;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:
                    ScrollTo(static_cast<int>(HIWORD(wParam)));
                    return 0;
                case SB_TOP:
                    ScrollTo(0);
                    return 0;
                case SB_BOTTOM:
                    ScrollTo(LineCount());
                    return 0;
                default:
                    return 0;
            }
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ScrollTo(topLine_ + (delta > 0 ? -3 : 3));
            return 0;
        }
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_UP:
                    ScrollTo(topLine_ - 1);
                    return 0;
                case VK_DOWN:
                    ScrollTo(topLine_ + 1);
                    return 0;
                case VK_PRIOR:
                    ScrollTo(topLine_ - VisibleLineCount());
                    return 0;
                case VK_NEXT:
                    ScrollTo(topLine_ + VisibleLineCount());
                    return 0;
                case VK_HOME:
                    ScrollTo(0);
                    return 0;
                case VK_END:
                    ScrollTo(LineCount());
                    return 0;
                default:
                    return 0;
            }
        case WM_GETTEXTLENGTH: {
            const std::wstring text = ToWide(VisibleText());
            return static_cast<LRESULT>(text.size());
        }
        case WM_GETTEXT: {
            // 与十六进制视图同一个理由：系统消息会被封送，别的进程能把面板内容读回去核对
            if (wParam == 0 || lParam == 0) {
                return 0;
            }
            const std::wstring text = ToWide(VisibleText());
            const std::size_t capacity = static_cast<std::size_t>(wParam);
            const std::size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
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

void MarkdownPreview::SetVisible(bool visible) const {
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
}

void MarkdownPreview::Resize(const RECT& area) const {
    if (hwnd_ != nullptr) {
        ::MoveWindow(hwnd_, area.left, area.top, area.right - area.left, area.bottom - area.top,
                     TRUE);
    }
}

}  // namespace npcc::shell
