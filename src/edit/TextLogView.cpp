#include "edit/TextLogView.h"

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "search/ByteMatcher.h"

namespace npcc::edit {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccTextLogView";

// 控件内部消息：查找线程跑完了（PostMessage 回 UI 线程再动界面）
constexpr UINT kMsgFindDone = WM_APP + 0x7A;
constexpr UINT_PTR kTimerIndex = 1;   // 行索引的扫描节拍（定时器 ID 只在窗口内有效）

constexpr int kLinesPerWheelNotch = 3;
constexpr std::uint64_t kFindWindowBytes = 16ull * 1024 * 1024;   // 一次匹配扫的窗口
constexpr std::uint64_t kFindOverlapBytes = 64ull * 1024;         // 跨界匹配的重叠
constexpr std::uint64_t kCopyLimitBytes = 1ull * 1024 * 1024;     // 单行复制/显示的解码上限
constexpr std::size_t   kWindowMatchLimit = 256;                  // 窗口内一次最多收的匹配数
constexpr std::uint64_t kCountLimit = 100000;                     // "计数"最多数到多少处

COLORREF ColorRef(std::uint32_t rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

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

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& bytes) {
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

std::wstring PadLeft(const std::wstring& text, int width) {
    if (static_cast<int>(text.size()) >= width) {
        return text;
    }
    return std::wstring(static_cast<std::size_t>(width - text.size()), L' ') + text;
}

int DigitCount(std::uint64_t value) {
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

}  // namespace

// 后台查找任务的全部状态。线程只碰这个对象和**只读**的映射数据，
// 不碰 TextLogView 本身——窗口先关、线程后收尾也不会踩到半死的对象。
struct TextLogView::FindTask {
    const char*            data = nullptr;
    std::uint64_t          size = 0;
    std::uint64_t          from = 0;      // 从哪开始（内容偏移）
    bool                   forward = true;
    bool                   wrap = true;
    bool                   countOnly = false;
    search::SearchConfig   config;
    HWND                   notify = nullptr;
    std::atomic<bool>      cancel{false};

    FindHit       hit;
    std::uint64_t count = 0;
    bool          truncated = false;
};

void TextLogView::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &TextLogView::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // 自己画背景（浅色/深色两套）
    wc.lpszClassName = kWindowClass;
    // 失败只可能是"已经注册过"（ERROR_CLASS_ALREADY_EXISTS）：重复调用无害
    ::RegisterClassExW(&wc);
}

TextLogView::~TextLogView() {
    CancelFindTask();
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK TextLogView::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                           LPARAM lParam) {
    auto* self = reinterpret_cast<TextLogView*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<TextLogView*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

bool TextLogView::Create(HWND parent, int controlId) {
    // 类没注册时 CreateWindowExW 会以"找不到窗口类"失败，而调用方
    // （DocumentManager 按文档创建视口）不该依赖"谁先替我注册过"。
    // 重复注册在这里无害：ERROR_CLASS_ALREADY_EXISTS 直接吞掉
    RegisterWindowClass(::GetModuleHandleW(nullptr));
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
    UpdateScrollBar();
    return true;
}

void TextLogView::Destroy() {
    CancelFindTask();
    StopIndexing();
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TextLogView::CreateFonts() {
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    LOGFONTW logFont{};
    // 等宽是硬要求：行号、偏移与正文要对成列，比例字体一进来整列就歪了
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

void TextLogView::SetPalette(const EditorPalette& palette) {
    palette_ = palette;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void TextLogView::SetCodePage(std::uint32_t codePage) {
    codePage_ = codePage;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void TextLogView::SetTabWidth(int width) {
    tabWidth_ = width > 0 ? width : 4;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void TextLogView::SetVisible(bool visible) const {
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
}

void TextLogView::Resize(const RECT& area) const {
    if (hwnd_ != nullptr) {
        ::MoveWindow(hwnd_, area.left, area.top, area.right - area.left,
                     area.bottom - area.top, TRUE);
    }
}

void TextLogView::Focus() const {
    if (hwnd_ != nullptr) {
        ::SetFocus(hwnd_);
    }
}

bool TextLogView::OpenFile(const std::wstring& path, std::uint64_t bomSkip, std::wstring* error) {
    CloseFile();
    if (!file_.Open(path)) {
        if (error != nullptr) {
            *error = file_.Error();
        }
        return false;
    }
    if (file_.Size() < bomSkip) {
        file_.Close();
        if (error != nullptr) {
            *error = L"文件比 BOM 还短，内容不完整。";
        }
        return false;
    }
    path_ = path;
    bomSkip_ = bomSkip;
    size_ = file_.Size() - bomSkip;
    topLine_ = 0;
    caretLine_ = 0;
    hitActive_ = false;
    hitStart_ = 0;
    hitEnd_ = 0;
    index_.Attach(size_ > 0 ? file_.Data() + bomSkip_ : file_.Data(), size_);
    UpdateScrollBar();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
    StartIndexing();
    return true;
}

void TextLogView::CloseFile() {
    StopIndexing();
    CancelFindTask();
    file_.Close();
    path_.clear();
    bomSkip_ = 0;
    size_ = 0;
    topLine_ = 0;
    caretLine_ = 0;
    hitActive_ = false;
    index_.Reset();
    if (hwnd_ != nullptr) {
        UpdateScrollBar();
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

bool TextLogView::WriteCopyTo(const std::wstring& path, std::wstring* error) const {
    if (!file_.Valid()) {
        if (error != nullptr) {
            *error = L"没有打开的源文件。";
        }
        return false;
    }
    // 先写临时文件再原子替换：中途失败不会在目标位置留下半截文件
    const std::wstring temp =
        path + L".notepadcc-tmp" + std::to_wstring(::GetCurrentProcessId());
    const HANDLE out = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = L"无法写入目标文件：" + path;
        }
        return false;
    }
    const char* cursor = file_.Data();
    std::uint64_t remaining = file_.Size();
    bool ok = true;
    while (remaining > 0) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::uint64_t>(remaining, 8ull * 1024 * 1024));
        DWORD written = 0;
        if (::WriteFile(out, cursor, chunk, &written, nullptr) == 0 || written == 0) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    ::CloseHandle(out);
    if (!ok) {
        ::DeleteFileW(temp.c_str());
        if (error != nullptr) {
            *error = L"写入副本失败：" + path;
        }
        return false;
    }
    if (::MoveFileExW(temp.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        ::DeleteFileW(temp.c_str());
        if (error != nullptr) {
            *error = L"替换目标文件失败：" + path;
        }
        return false;
    }
    return true;
}

void TextLogView::StartIndexing() {
    if (hwnd_ != nullptr && !index_.Complete()) {
        ::SetTimer(hwnd_, kTimerIndex, 16, nullptr);
    }
    if (statusCallback_) {
        statusCallback_();
    }
}

void TextLogView::StopIndexing() {
    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kTimerIndex);
    }
}

int TextLogView::VisibleRowCount() const {
    if (hwnd_ == nullptr || charHeight_ <= 0) {
        return 1;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int rows = (client.bottom - client.top) / charHeight_;
    return rows > 0 ? rows : 1;
}

int TextLogView::GutterColumns() const {
    const int numberCols = std::max(3, DigitCount(index_.LineCount()));
    const int offsetCols = FileSize() > 0xFFFFFFFFull ? 12 : 8;
    return 1 + numberCols + 2 + offsetCols + 2;
}

std::uint64_t TextLogView::MaxTopLine() const {
    const std::uint64_t lines = index_.LineCount();
    const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());
    return lines > rows ? lines - rows : 0;
}

void TextLogView::UpdateScrollBar() const {
    if (hwnd_ == nullptr) {
        return;
    }
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    const std::uint64_t lines = index_.LineCount();
    info.nMax = lines > 0 ? static_cast<int>(std::min<std::uint64_t>(lines - 1, 0x3FFFFFFF)) : 0;
    info.nPage = static_cast<UINT>(VisibleRowCount());
    info.nPos = static_cast<int>(std::min<std::uint64_t>(topLine_, 0x3FFFFFFF));
    ::SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
}

void TextLogView::ScrollTo(std::uint64_t topLine) {
    const std::uint64_t clamped = std::min(topLine, MaxTopLine());
    if (clamped == topLine_) {
        return;
    }
    topLine_ = clamped;
    UpdateScrollBar();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
    if (statusCallback_) {
        statusCallback_();
    }
}

void TextLogView::SetTopLine(std::uint64_t line) {
    ScrollTo(line);
}

void TextLogView::GoToLine(std::uint64_t line, bool scroll) {
    const std::uint64_t clamped =
        line < 1 ? 0 : std::min<std::uint64_t>(line - 1, index_.LineCount() - 1);
    caretLine_ = clamped;
    if (scroll) {
        // 与十六进制视图的"跳到偏移"一个态度：目标滚到屏幕第一行，用户是冲着它来的
        topLine_ = std::min(clamped, MaxTopLine());
        UpdateScrollBar();
    }
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
    if (statusCallback_) {
        statusCallback_();
    }
}

void TextLogView::EnsureCaretVisible() {
    const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());
    if (caretLine_ < topLine_) {
        ScrollTo(caretLine_);
    } else if (caretLine_ >= topLine_ + rows) {
        ScrollTo(caretLine_ - rows + 1);
    }
}

void TextLogView::SetCaretLine(std::uint64_t line, bool notify) {
    const std::uint64_t clamped = std::min(line, index_.LineCount() - 1);
    if (clamped == caretLine_ && !notify) {
        return;
    }
    caretLine_ = clamped;
    EnsureCaretVisible();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
    if (notify && statusCallback_) {
        statusCallback_();
    }
}

void TextLogView::ClearHit() {
    if (!hitActive_) {
        return;
    }
    hitActive_ = false;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

std::uint64_t TextLogView::ContentOffsetOfLine(std::uint64_t line) const {
    std::uint64_t offset = 0;
    if (!index_.LineStart(line, &offset)) {
        return 0;
    }
    return offset;
}

std::uint64_t TextLogView::CurrentOffset() const {
    return ContentOffsetOfLine(caretLine_);
}

std::wstring TextLogView::CurrentLineText() const {
    std::uint64_t start = 0;
    std::uint64_t contentEnd = 0;
    std::uint64_t nextStart = 0;
    if (!index_.LineRange(caretLine_, &start, &contentEnd, &nextStart)) {
        return {};
    }
    return DecodeRange(start, contentEnd, kCopyLimitBytes);
}

std::wstring TextLogView::DecodeRange(std::uint64_t start, std::uint64_t end,
                                      std::uint64_t byteLimit, int startColumn) const {
    if (!file_.Valid() || end <= start || start >= size_) {
        return {};
    }
    const std::uint64_t capped = std::min(end, std::min(start + byteLimit, size_));
    const std::size_t bytes = static_cast<std::size_t>(capped - start);
    if (bytes == 0) {
        return {};
    }
    const char* source = file_.Data() + bomSkip_ + start;
    std::wstring out(bytes + 1, L'\0');
    const int written = ::MultiByteToWideChar(codePage_, 0, source, static_cast<int>(bytes),
                                              out.data(), static_cast<int>(out.size()));
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written));

    // Tab 展开：按屏幕列对齐（每个 DecodeRange 调用都要知道自己从第几列开始，
    // 不然"命中高亮的列"与"正文的列"对不上）
    std::wstring expanded;
    expanded.reserve(out.size());
    int column = startColumn;
    for (const wchar_t ch : out) {
        if (ch == L'\t') {
            const int width = tabWidth_ > 0 ? tabWidth_ : 4;
            do {
                expanded.push_back(L' ');
                ++column;
            } while (column % width != 0);
        } else {
            expanded.push_back(ch);
            ++column;
        }
    }
    return expanded;
}

std::wstring TextLogView::FormatOffset(std::uint64_t contentOffset) const {
    std::uint64_t shown = contentOffset + bomSkip_;
    const int width = FileSize() > 0xFFFFFFFFull ? 12 : 8;
    std::wstring out(static_cast<std::size_t>(width), L'0');
    const wchar_t* digits = L"0123456789ABCDEF";
    for (int i = width - 1; i >= 0 && shown != 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[shown & 0xF];
        shown >>= 4;
    }
    return out;
}

std::uint64_t TextLogView::TextByteLimit() const {
    int columns = 200;
    if (hwnd_ != nullptr && charWidth_ > 0) {
        RECT client{};
        ::GetClientRect(hwnd_, &client);
        columns = client.right / charWidth_;
    }
    if (columns < 40) {
        columns = 40;
    }
    // UTF-8 一个字最多 4 字节；再给一截余量，保证"看得到的都解出来"
    return static_cast<std::uint64_t>(columns) * 4 + 64;
}

bool TextLogView::LineColumns(std::uint64_t line, std::wstring* number, std::wstring* offset,
                              std::wstring* text) const {
    std::uint64_t start = 0;
    if (!index_.LineStart(line, &start)) {
        return false;
    }
    std::uint64_t contentEnd = 0;
    std::uint64_t nextStart = 0;
    index_.SpanAt(start, &contentEnd, &nextStart);
    const int numberCols = std::max(3, DigitCount(index_.LineCount()));
    *number = PadLeft(std::to_wstring(line + 1), numberCols);
    *offset = FormatOffset(start);
    *text = DecodeRange(start, contentEnd, TextByteLimit());
    return true;
}

void TextLogView::Paint(HDC dc, const RECT& client) {
    const bool dark = palette_.dark;
    const COLORREF background = ColorRef(dark ? 0x1E1E1E : 0xFFFFFF);
    const COLORREF textColour = ColorRef(dark ? 0xE6E6E6 : 0x1E1E1E);
    const COLORREF gutterFore = ColorRef(palette_.lineNumberFore);
    const COLORREF gutterBack = ColorRef(palette_.lineNumberBack);
    const COLORREF caretBack = ColorRef(palette_.caretLineBack);
    const COLORREF hitBack = ColorRef(palette_.selectionBack);

    const HBRUSH backgroundBrush = ::CreateSolidBrush(background);
    ::FillRect(dc, &client, backgroundBrush);
    ::DeleteObject(backgroundBrush);

    const HGDIOBJ previousFont = ::SelectObject(dc, font_);
    ::SetBkMode(dc, TRANSPARENT);

    if (!file_.Valid()) {
        ::SetTextColor(dc, gutterFore);
        const wchar_t* hint = L"（没有打开文件）";
        ::TextOutW(dc, charWidth_, charHeight_, hint, static_cast<int>(::wcslen(hint)));
        ::SelectObject(dc, previousFont);
        return;
    }

    const int numberCols = std::max(3, DigitCount(index_.LineCount()));
    const int textLeft = GutterColumns() * charWidth_;
    const RECT gutterRect{0, 0, textLeft, client.bottom};
    const HBRUSH gutterBrush = ::CreateSolidBrush(gutterBack);
    ::FillRect(dc, &gutterRect, gutterBrush);
    ::DeleteObject(gutterBrush);

    const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());
    const std::uint64_t byteLimit = TextByteLimit();
    std::uint64_t offset = 0;
    bool offsetOk = index_.LineStart(topLine_, &offset);
    for (std::uint64_t row = 0; row < rows && offsetOk; ++row) {
        const std::uint64_t line = topLine_ + row;
        std::uint64_t contentEnd = 0;
        std::uint64_t nextStart = 0;
        index_.SpanAt(offset, &contentEnd, &nextStart);
        const std::wstring text = DecodeRange(offset, contentEnd, byteLimit);
        const int y = static_cast<int>(row) * charHeight_;

        // 光标行先铺底（整行，连文字区）
        if (line == caretLine_) {
            const RECT caretRect{textLeft, y, client.right, y + charHeight_};
            const HBRUSH caretBrush = ::CreateSolidBrush(caretBack);
            ::FillRect(dc, &caretRect, caretBrush);
            ::DeleteObject(caretBrush);
        }

        // 命中高亮：只画与本行相交的那一段
        if (hitActive_ && hitEnd_ > hitStart_ && hitStart_ < contentEnd && hitEnd_ > offset) {
            const std::uint64_t from = std::max(hitStart_, offset);
            const std::uint64_t to = std::min(hitEnd_, contentEnd);
            const std::wstring prefix = DecodeRange(offset, from, byteLimit);
            const int column = static_cast<int>(prefix.size());
            const std::wstring hitPart = DecodeRange(from, to, byteLimit, column);
            int columnEnd = column + static_cast<int>(hitPart.size());
            if (columnEnd > static_cast<int>(text.size())) {
                columnEnd = static_cast<int>(text.size());
            }
            if (column < columnEnd) {
                const RECT hitRect{textLeft + column * charWidth_, y,
                                   textLeft + columnEnd * charWidth_, y + charHeight_};
                const HBRUSH hitBrush = ::CreateSolidBrush(hitBack);
                ::FillRect(dc, &hitRect, hitBrush);
                ::DeleteObject(hitBrush);
            }
        }

        ::SetTextColor(dc, gutterFore);
        const std::wstring number = PadLeft(std::to_wstring(line + 1), numberCols);
        ::TextOutW(dc, charWidth_, y, number.c_str(), static_cast<int>(number.size()));
        const std::wstring offsetText = FormatOffset(offset);
        ::TextOutW(dc, (1 + numberCols + 2) * charWidth_, y, offsetText.c_str(),
                   static_cast<int>(offsetText.size()));

        ::SetTextColor(dc, textColour);
        const RECT clip{textLeft, y, client.right, y + charHeight_};
        ::ExtTextOutW(dc, textLeft, y, ETO_CLIPPED, &clip, text.c_str(),
                      static_cast<int>(text.size()), nullptr);

        offset = nextStart;
    }

    // 索引没建完：右下角挂个进度，别让用户以为"文件就这么长"
    if (!index_.Complete()) {
        const int percent = static_cast<int>(index_.Progress() * 100.0 + 0.5);
        const std::wstring progress = L"正在建立索引 " + std::to_wstring(percent) + L"%…";
        const int width = static_cast<int>(progress.size() + 2) * charWidth_;
        const RECT progressRect{client.right - width, client.bottom - charHeight_, client.right,
                                client.bottom};
        const HBRUSH progressBrush = ::CreateSolidBrush(gutterBack);
        ::FillRect(dc, &progressRect, progressBrush);
        ::DeleteObject(progressBrush);
        ::SetTextColor(dc, gutterFore);
        ::TextOutW(dc, progressRect.left + charWidth_, progressRect.top, progress.c_str(),
                   static_cast<int>(progress.size()));
    }

    ::SelectObject(dc, previousFont);
}

void TextLogView::HandleKey(WPARAM key) {
    if (!file_.Valid()) {
        return;
    }
    const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());
    switch (key) {
        case VK_UP:
            SetCaretLine(caretLine_ > 0 ? caretLine_ - 1 : 0, true);
            return;
        case VK_DOWN:
            SetCaretLine(caretLine_ + 1, true);
            return;
        case VK_PRIOR:
            SetCaretLine(caretLine_ >= rows ? caretLine_ - rows : 0, true);
            return;
        case VK_NEXT:
            SetCaretLine(caretLine_ + rows, true);
            return;
        case VK_HOME:
            SetCaretLine(0, true);
            return;
        case VK_END:
            SetCaretLine(index_.LineCount() - 1, true);
            return;
        default:
            return;
    }
}

void TextLogView::HandleClick(LPARAM position) {
    if (!file_.Valid() || charHeight_ <= 0) {
        return;
    }
    const int y = GET_Y_LPARAM(position);
    if (y < 0) {
        return;
    }
    SetCaretLine(topLine_ + static_cast<std::uint64_t>(y / charHeight_), true);
}

std::string TextLogView::VisibleText() const {
    if (!file_.Valid()) {
        return {};
    }
    const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());
    std::string out;
    for (std::uint64_t row = 0; row < rows; ++row) {
        std::wstring number;
        std::wstring offset;
        std::wstring text;
        if (!LineColumns(topLine_ + row, &number, &offset, &text)) {
            break;
        }
        const std::wstring line = L" " + number + L"  " + offset + L"  " + text;
        if (!out.empty()) {
            out += "\r\n";
        }
        out += ToUtf8(line);
    }
    return out;
}

// ---- 查找 ----

void TextLogView::SetFindCallback(std::function<void(const FindHit&)> callback) {
    findCallback_ = std::move(callback);
}

void TextLogView::SetCountCallback(
    std::function<void(std::uint64_t, bool, const std::wstring&)> callback) {
    countCallback_ = std::move(callback);
}

void TextLogView::SetStatusCallback(std::function<void()> callback) {
    statusCallback_ = std::move(callback);
}

void TextLogView::SetEditAttemptCallback(std::function<void()> callback) {
    editAttempt_ = std::move(callback);
}

void TextLogView::FindNext(const search::SearchConfig& config, bool forward, bool wrap) {
    if (!file_.Valid()) {
        return;
    }
    if (!index_.Complete()) {
        FindHit hit;
        hit.error = L"正在建立索引（" +
                    std::to_wstring(static_cast<int>(index_.Progress() * 100.0 + 0.5)) +
                    L"%），建完再找。";
        if (findCallback_) {
            findCallback_(hit);
        }
        return;
    }
    StartFindTask(config, forward, wrap, false);
}

void TextLogView::CountMatches(const search::SearchConfig& config) {
    if (!file_.Valid()) {
        return;
    }
    if (!index_.Complete()) {
        if (countCallback_) {
            countCallback_(0, false,
                           L"正在建立索引（" +
                               std::to_wstring(
                                   static_cast<int>(index_.Progress() * 100.0 + 0.5)) +
                               L"%），建完再数。");
        }
        return;
    }
    StartFindTask(config, true, true, true);
}

void TextLogView::StartFindTask(const search::SearchConfig& config, bool forward, bool wrap,
                                bool countOnly) {
    CancelFindTask();
    auto task = std::make_shared<FindTask>();
    task->data = file_.Data() + bomSkip_;
    task->size = size_;
    task->config = config;
    task->forward = forward;
    task->wrap = wrap;
    task->countOnly = countOnly;
    task->notify = hwnd_;
    if (countOnly) {
        task->from = 0;
    } else if (forward) {
        task->from = hitActive_ ? hitEnd_ : CurrentOffset();
    } else {
        task->from = hitActive_ ? hitStart_ : CurrentOffset();
    }
    findTask_ = task;
    findThread_ = std::thread(&TextLogView::RunFindTask, task);
}

void TextLogView::CancelFindTask() {
    if (findThread_.joinable()) {
        if (findTask_) {
            findTask_->cancel = true;
        }
        findThread_.join();
    }
    findTask_.reset();
}

void TextLogView::RunFindTask(std::shared_ptr<FindTask> task) {
    search::ByteMatcher matcher;
    search::SearchOutcome outcome;
    if (!matcher.Prepare(task->config, task->config.utfMode, task->config.codePage, outcome)) {
        task->hit.error = outcome.message;
        if (task->notify != nullptr) {
            ::PostMessageW(task->notify, kMsgFindDone, 0, 0);
        }
        return;
    }

    const std::uint64_t fileSize = task->size;
    std::vector<search::MatchRange> matches;
    bool windowTruncated = false;

    if (task->countOnly) {
        std::uint64_t pos = 0;
        while (pos < fileSize) {
            if (task->cancel) {
                return;
            }
            const std::uint64_t coreEnd = std::min(pos + kFindWindowBytes, fileSize);
            const std::uint64_t readEnd = std::min(coreEnd + kFindOverlapBytes, fileSize);
            // 一个 16 MB 窗口里可能有几十万处匹配，而一次 FindAll 最多只收
            // kWindowMatchLimit 处——收满了就从最后一处的末尾**续扫**同一窗口，
            // 直到越过 coreEnd。把"窗口内收满"当成"全文件统计上限"是错的
            //（分段回归实测：71 MB、每行一处匹配的文件被误报成"已达统计上限 256 处"）
            std::uint64_t scanFrom = pos;
            while (scanFrom < coreEnd) {
                if (task->cancel) {
                    return;
                }
                matches.clear();
                windowTruncated = false;
                const std::size_t remaining = static_cast<std::size_t>(
                    std::min<std::uint64_t>(kCountLimit - task->count, kWindowMatchLimit));
                if (remaining == 0) {
                    task->truncated = true;
                    break;
                }
                const bool ok = matcher.FindAll(
                    std::string_view(task->data + scanFrom,
                                     static_cast<std::size_t>(readEnd - scanFrom)),
                    matches, remaining, windowTruncated, outcome);
                if (!ok) {
                    task->hit.error = outcome.message;
                    break;
                }
                bool moreInWindow = windowTruncated;
                // 注意：match 的偏移是相对**这一批的起点**（viewStart）的，
                // 而 scanFrom 会随着逐处计数往后走——两者必须分开，
                // 否则相对偏移会被叠加到已经推进过的游标上（第一版就是这么错的：
                // 第二个匹配的绝对位置被算成 viewStart 之后的又一段，直接越界跳出）
                const std::uint64_t viewStart = scanFrom;
                for (const search::MatchRange& match : matches) {
                    const std::uint64_t start =
                        viewStart + static_cast<std::uint64_t>(match.start);
                    if (start >= coreEnd) {
                        moreInWindow = false;   // 剩下的都属于重叠区：下一个窗口数
                        break;
                    }
                    ++task->count;
                    const std::uint64_t width = match.end > match.start
                                                    ? static_cast<std::uint64_t>(match.end -
                                                                                 match.start)
                                                    : 1;
                    scanFrom = start + width;
                }
                if (!moreInWindow) {
                    break;
                }
            }
            if (!task->hit.error.empty() || task->count >= kCountLimit) {
                break;
            }
            if (coreEnd >= fileSize) {
                break;
            }
            pos = coreEnd;
        }
        if (task->count >= kCountLimit) {
            task->truncated = true;
        }
    } else if (task->forward) {
        std::uint64_t pos = std::min(task->from, fileSize);
        bool wrapped = false;
        while (true) {
            if (task->cancel) {
                return;
            }
            if (pos >= fileSize || (wrapped && pos >= task->from)) {
                if (!task->wrap || wrapped) {
                    break;   // 没找到（回绕一圈也没有）
                }
                wrapped = true;
                pos = 0;
                continue;
            }
            const std::uint64_t coreEnd = std::min(pos + kFindWindowBytes, fileSize);
            const std::uint64_t readEnd = std::min(coreEnd + kFindOverlapBytes, fileSize);
            matches.clear();
            if (!matcher.FindAll(
                    std::string_view(task->data + pos, static_cast<std::size_t>(readEnd - pos)),
                    matches, kWindowMatchLimit, windowTruncated, outcome)) {
                task->hit.error = outcome.message;
                break;
            }
            for (const search::MatchRange& match : matches) {
                const std::uint64_t start = pos + static_cast<std::uint64_t>(match.start);
                if (start >= coreEnd) {
                    break;
                }
                task->hit.found = true;
                task->hit.matchStart = start;
                task->hit.matchEnd = pos + static_cast<std::uint64_t>(match.end);
                task->hit.wrapped = wrapped;
                break;
            }
            if (task->hit.found) {
                break;
            }
            if (coreEnd >= fileSize) {
                if (!task->wrap || wrapped) {
                    break;
                }
                wrapped = true;
                pos = 0;
                continue;
            }
            pos = coreEnd;
        }
    } else {
        std::uint64_t pos = std::min(task->from, fileSize);
        bool wrapped = false;
        while (true) {
            if (task->cancel) {
                return;
            }
            if (pos == 0 || (wrapped && pos <= task->from)) {
                if (!task->wrap || wrapped) {
                    break;
                }
                wrapped = true;
                pos = fileSize;
                continue;
            }
            const std::uint64_t coreStart = pos > kFindWindowBytes ? pos - kFindWindowBytes : 0;
            const std::uint64_t readEnd = std::min(pos + kFindOverlapBytes, fileSize);
            matches.clear();
            if (!matcher.FindAll(
                    std::string_view(task->data + coreStart,
                                     static_cast<std::size_t>(readEnd - coreStart)),
                    matches, kWindowMatchLimit, windowTruncated, outcome)) {
                task->hit.error = outcome.message;
                break;
            }
            // 要"最后一个 start < pos 的匹配"：从后往前挑
            for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
                const std::uint64_t start = coreStart + static_cast<std::uint64_t>(it->start);
                if (start >= pos || start < coreStart) {
                    continue;
                }
                task->hit.found = true;
                task->hit.matchStart = start;
                task->hit.matchEnd = coreStart + static_cast<std::uint64_t>(it->end);
                task->hit.wrapped = wrapped;
                break;
            }
            if (task->hit.found) {
                break;
            }
            if (coreStart == 0) {
                if (!task->wrap || wrapped) {
                    break;
                }
                wrapped = true;
                pos = fileSize;
                continue;
            }
            pos = coreStart;
        }
    }

    if (task->notify != nullptr && !task->cancel) {
        ::PostMessageW(task->notify, kMsgFindDone, 0, 0);
    }
}

void TextLogView::ApplyFindResult(const FindTask& task) {
    if (task.countOnly) {
        if (countCallback_) {
            countCallback_(task.count, task.truncated, task.hit.error);
        }
        return;
    }
    FindHit hit = task.hit;
    if (hit.found) {
        hitActive_ = true;
        hitStart_ = hit.matchStart;
        hitEnd_ = hit.matchEnd;
        std::uint64_t line = 0;
        if (index_.LineOfOffset(hit.matchStart, &line)) {
            hit.line = line;
        }
        // 与"跳到偏移"一个态度：命中滚到屏幕第一行
        GoToLine(hit.line + 1, true);
    }
    if (findCallback_) {
        findCallback_(hit);
    }
}LRESULT TextLogView::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
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
        case WM_TIMER:
            if (wParam == kTimerIndex) {
                const bool done = index_.ScanNextChunk();
                UpdateScrollBar();
                ::InvalidateRect(hwnd_, nullptr, FALSE);
                if (done) {
                    StopIndexing();
                }
                if (statusCallback_) {
                    statusCallback_();
                }
                return 0;
            }
            break;
        case kMsgFindDone: {
            if (findThread_.joinable()) {
                findThread_.join();
            }
            auto task = findTask_;
            findTask_.reset();
            if (task) {
                ApplyFindResult(*task);
            }
            return 0;
        }
        case WM_VSCROLL: {
            const int request = LOWORD(wParam);
            const std::uint64_t rows = static_cast<std::uint64_t>(VisibleRowCount());
            switch (request) {
                case SB_LINEUP:
                    ScrollTo(topLine_ > 0 ? topLine_ - 1 : 0);
                    return 0;
                case SB_LINEDOWN:
                    ScrollTo(topLine_ + 1);
                    return 0;
                case SB_PAGEUP:
                    ScrollTo(topLine_ > rows ? topLine_ - rows : 0);
                    return 0;
                case SB_PAGEDOWN:
                    ScrollTo(topLine_ + rows);
                    return 0;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO info{};
                    info.cbSize = sizeof(info);
                    info.fMask = SIF_TRACKPOS;
                    ::GetScrollInfo(hwnd_, SB_VERT, &info);
                    ScrollTo(static_cast<std::uint64_t>(info.nTrackPos));
                    return 0;
                }
                case SB_TOP:
                    ScrollTo(0);
                    return 0;
                case SB_BOTTOM:
                    ScrollTo(MaxTopLine());
                    return 0;
                default:
                    return 0;
            }
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int lines = delta > 0 ? -kLinesPerWheelNotch : kLinesPerWheelNotch;
            const std::int64_t target = static_cast<std::int64_t>(topLine_) + lines;
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
        case WM_KEYDOWN:
            HandleKey(wParam);
            return 0;
        case WM_CHAR: {
            // 走到这里说明用户想往只读视图里打字：交给外壳去问"开个副本？"
            const wchar_t ch = static_cast<wchar_t>(wParam);
            if (ch >= 0x20 || ch == L'\t' || ch == L'\r' || ch == L'\b') {
                if (editAttempt_) {
                    editAttempt_();
                }
            }
            return 0;
        }
        case WM_PASTE:
        case WM_CUT:
            if (editAttempt_) {
                editAttempt_();
            }
            return 0;
        case WM_GETTEXTLENGTH: {
            const std::wstring text = Utf8ToWide(VisibleText());
            return static_cast<LRESULT>(text.size());
        }
        case WM_GETTEXT: {
            // 系统消息（< WM_USER）会被 OS 封送，所以别的进程也能把这几行读回去。
            // 这是这个控件能被端到端验证的原因
            if (wParam == 0 || lParam == 0) {
                return 0;
            }
            const std::wstring text = Utf8ToWide(VisibleText());
            const std::size_t capacity = static_cast<std::size_t>(wParam);
            const std::size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
            std::wmemcpy(reinterpret_cast<wchar_t*>(lParam), text.c_str(), count);
            reinterpret_cast<wchar_t*>(lParam)[count] = L'\0';
            return static_cast<LRESULT>(count);
        }
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace npcc::edit
