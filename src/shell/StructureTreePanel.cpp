#include "shell/StructureTreePanel.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>

#include "core/Localization.h"
#include "infra/Log.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccStructureTree";
constexpr int kIndentCells = 2;      // 每一层的缩进（字符数）

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

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          need, nullptr, nullptr);
    return out;
}

// 长值截到 limit 个字节再加省略号；不把 UTF-8 序列截成半个字符（续字节是 10xxxxxx）
std::string Ellipsize(std::string_view text, std::size_t limit = 80) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    std::string out(text.substr(0, cut));
    out += "…";
    return out;
}

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
    return 1;
}

// 显示宽度（中文两格）——与差异渲染那套同一个口径，缩进才对得齐
int CellsOf(std::string_view text) {
    int cells = 0;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        const std::size_t length = Utf8Length(lead);
        std::uint32_t code = lead;
        if (length == 3 && index + 2 < text.size()) {
            code = (static_cast<std::uint32_t>(lead & 0x0Fu) << 12) |
                   (static_cast<std::uint32_t>(
                        static_cast<unsigned char>(text[index + 1]) & 0x3Fu)
                    << 6) |
                   static_cast<std::uint32_t>(
                       static_cast<unsigned char>(text[index + 2]) & 0x3Fu);
        }
        const bool wide = (code >= 0x2E80 && code <= 0x303E) ||
                          (code >= 0x3041 && code <= 0x33FF) ||
                          (code >= 0x4E00 && code <= 0x9FFF) ||
                          (code >= 0xAC00 && code <= 0xD7A3) ||
                          (code >= 0xF900 && code <= 0xFAFF) ||
                          (code >= 0xFF00 && code <= 0xFF60) ||
                          (code >= 0xFFE0 && code <= 0xFFE6);
        cells += wide ? 2 : 1;
        index += length;
    }
    return cells;
}

}  // namespace

void StructureTreePanel::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &StructureTreePanel::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);
}

LRESULT CALLBACK StructureTreePanel::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    auto* self = reinterpret_cast<StructureTreePanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<StructureTreePanel*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

bool StructureTreePanel::Create(HWND parent, int controlId) {
    dpi_ = DpiOf(parent);
    hwnd_ = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0,
                              10, 10, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }
    CreateFonts();
    return true;
}

void StructureTreePanel::CreateFonts() {
    const auto make = [this](int weight) {
        LOGFONTW logFont{};
        ::wcscpy_s(logFont.lfFaceName, L"Consolas");
        logFont.lfHeight = -::MulDiv(10, dpi_, 72);
        logFont.lfWeight = weight;
        logFont.lfCharSet = DEFAULT_CHARSET;
        return ::CreateFontIndirectW(&logFont);
    };
    if (font_ != nullptr) {
        ::DeleteObject(font_);
    }
    if (boldFont_ != nullptr) {
        ::DeleteObject(boldFont_);
    }
    font_ = make(FW_NORMAL);
    boldFont_ = make(FW_BOLD);
    if (font_ == nullptr) {
        font_ = static_cast<HFONT>(::GetStockObject(ANSI_FIXED_FONT));
    }
    if (boldFont_ == nullptr) {
        boldFont_ = font_;
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

void StructureTreePanel::SetDark(bool dark) {
    dark_ = dark;
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void StructureTreePanel::Clear() {
    source_.clear();
    tree_ = core::JsonTreeResult{};
    xmlTree_ = core::XmlTreeResult{};
    valid_ = false;
    nodeCount_ = 0;
    rows_.clear();
    collapsedJson_.clear();
    collapsedXml_.clear();
    topLine_ = 0;
    UpdateScrollBar();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void StructureTreePanel::SetJson(std::string utf8) {
    source_ = std::move(utf8);
    isXml_ = false;
    // 折起来的状态在换内容时就该丢掉：节点指针全变了，留着等于随机折几个
    collapsedJson_.clear();
    collapsedXml_.clear();
    tree_ = core::ParseJsonTree(source_);
    valid_ = tree_.ok;
    nodeCount_ = tree_.ok ? tree_.nodeCount : 0;
    Rebuild();
}

void StructureTreePanel::SetXml(std::string utf8) {
    source_ = std::move(utf8);
    isXml_ = true;
    collapsedJson_.clear();
    collapsedXml_.clear();
    xmlTree_ = core::ParseXmlTree(source_);
    valid_ = xmlTree_.ok;
    nodeCount_ = xmlTree_.ok ? xmlTree_.nodeCount : 0;
    Rebuild();
}


void StructureTreePanel::Rebuild() {
    rows_.clear();
    if (valid_) {
        if (isXml_) {
            FlattenXml(xmlTree_.root, 0, true);
        } else {
            FlattenJson(tree_.root, 0);
        }
    }
    topLine_ = 0;
    UpdateScrollBar();
    if (hwnd_ != nullptr) {
        ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}


bool StructureTreePanel::ContainerOf(const Row& row) const {
    if (row.xml != nullptr) {
        return row.xml->kind == core::XmlNodeKind::Element && !row.xml->children.empty();
    }
    return row.json->kind == core::JsonKind::Object || row.json->kind == core::JsonKind::Array;
}

bool StructureTreePanel::Collapsed(const Row& row) const {
    if (row.xml != nullptr) {
        for (const core::XmlNode* item : collapsedXml_) {
            if (item == row.xml) {
                return true;
            }
        }
        return false;
    }
    for (const core::JsonNode* item : collapsedJson_) {
        if (item == row.json) {
            return true;
        }
    }
    return false;
}

void StructureTreePanel::Toggle(const Row& row) {
    if (row.xml != nullptr) {
        for (std::size_t index = 0; index < collapsedXml_.size(); ++index) {
            if (collapsedXml_[index] == row.xml) {
                collapsedXml_.erase(collapsedXml_.begin() + static_cast<std::ptrdiff_t>(index));
                Rebuild();
                return;
            }
        }
        collapsedXml_.push_back(row.xml);
        Rebuild();
        return;
    }
    for (std::size_t index = 0; index < collapsedJson_.size(); ++index) {
        if (collapsedJson_[index] == row.json) {
            collapsedJson_.erase(collapsedJson_.begin() + static_cast<std::ptrdiff_t>(index));
            Rebuild();
            return;
        }
    }
    collapsedJson_.push_back(row.json);
    Rebuild();
}

void StructureTreePanel::FlattenJson(const core::JsonNode& node, int depth) {
    Row row;
    row.json = &node;
    row.depth = depth;
    rows_.push_back(row);
    const bool container =
        node.kind == core::JsonKind::Object || node.kind == core::JsonKind::Array;
    if (!container || Collapsed(row)) {
        return;
    }
    for (const core::JsonNode& child : node.children) {
        FlattenJson(child, depth + 1);
    }
}

void StructureTreePanel::FlattenXml(const core::XmlNode& node, int depth, bool rootDocument) {
    Row row;
    row.xml = &node;
    row.depth = depth;
    if (!rootDocument) {
        rows_.push_back(row);
    }
    // 文档根自己不是元素，但它的孩子要照常铺开（根不进显示行，也永远不会被折叠）
    const bool container =
        (node.kind == core::XmlNodeKind::Element || node.kind == core::XmlNodeKind::Document) &&
        !node.children.empty();
    if (!container || (!rootDocument && Collapsed(row))) {
        return;
    }
    for (const core::XmlNode& child : node.children) {
        FlattenXml(child, rootDocument ? 0 : depth + 1, false);
    }
}


int StructureTreePanel::VisibleLineCount() const {
    if (hwnd_ == nullptr || charHeight_ <= 0) {
        return 1;
    }
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int rows = (client.bottom - client.top) / charHeight_;
    return rows > 0 ? rows : 1;
}

void StructureTreePanel::UpdateScrollBar() const {
    if (hwnd_ == nullptr) {
        return;
    }
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = rows_.empty() ? 0 : static_cast<int>(rows_.size()) - 1;
    info.nPage = static_cast<UINT>(VisibleLineCount());
    info.nPos = topLine_;
    ::SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
}

void StructureTreePanel::ScrollTo(int topLine) {
    const int maxTop = std::max(0, static_cast<int>(rows_.size()) - VisibleLineCount());
    const int clamped = std::min(std::max(topLine, 0), maxTop);
    if (clamped == topLine_) {
        return;
    }
    topLine_ = clamped;
    UpdateScrollBar();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

std::string StructureTreePanel::RowText(const Row& row) const {
    return row.xml != nullptr ? XmlRowText(row) : JsonRowText(row);
}

std::string StructureTreePanel::JsonRowText(const Row& row) const {
    const core::JsonNode& node = *row.json;
    std::string text(static_cast<std::size_t>(row.depth) * kIndentCells, ' ');
    const bool container = ContainerOf(row);
    if (container) {
        // 折叠记号用 ASCII 的 v/>：等宽字体里宽度确定，跨字体也不会变成方框
        text += Collapsed(row) ? "> " : "v ";
    } else {
        text += "  ";
    }
    if (!node.name.empty()) {
        text += "\"";
        text += node.name;
        text += "\": ";
    }
    if (node.kind == core::JsonKind::Object || node.kind == core::JsonKind::Array) {
        const char open = node.kind == core::JsonKind::Object ? '{' : '[';
        const char close = node.kind == core::JsonKind::Object ? '}' : ']';
        if (Collapsed(row) && node.count > 0) {
            text.push_back(open);
            text += " " + std::to_string(node.count) + " ";
            text.push_back(close);
        } else {
            text.push_back(open);
        }
        return text;
    }
    if (node.kind == core::JsonKind::String) {
        text += "\"";
        text += node.value;
        text += "\"";
        return text;
    }
    text += node.value;
    return text;
}

std::string StructureTreePanel::XmlRowText(const Row& row) const {
    const core::XmlNode& node = *row.xml;
    std::string text(static_cast<std::size_t>(row.depth) * kIndentCells, ' ');
    const bool container = ContainerOf(row);
    if (container) {
        text += Collapsed(row) ? "> " : "v ";
    } else {
        text += "  ";
    }
    switch (node.kind) {
        case core::XmlNodeKind::Element:
            text += "<";
            text += node.name;
            if (container && Collapsed(row)) {
                text += " ... ";
                text += std::to_string(node.children.size());
                text += " 项";
            }
            text += ">";
            break;
        case core::XmlNodeKind::Attribute:
            text += "@";
            text += node.name;
            text += " = \"";
            text += Ellipsize(node.value);
            text += "\"";
            break;
        case core::XmlNodeKind::Text:
            text += "\"";
            text += Ellipsize(node.value);
            text += "\"";
            break;
        case core::XmlNodeKind::CData:
            text += "CDATA: ";
            text += Ellipsize(node.value);
            break;
        case core::XmlNodeKind::Comment:
            text += "<!-- ";
            text += Ellipsize(node.value);
            text += " -->";
            break;
        case core::XmlNodeKind::PI:
            text += "<?";
            text += node.name;
            if (!node.value.empty()) {
                text += " ";
                text += Ellipsize(node.value);
            }
            text += "?>";
            break;
        case core::XmlNodeKind::Doctype:
            text += Ellipsize(node.value);
            break;
        case core::XmlNodeKind::Document:
            break;   // 文档根不进显示行
    }
    return text;
}


std::string StructureTreePanel::VisibleText() const {
    std::string text;
    if (!valid_) {
        // 不是合法 JSON / XML：把原因（含行列）直接显示出来——与其它几块“失败要说清楚”一致
        if (source_.empty()) {
            return isXml_ ? ToUtf8(core::Tr(L"xml.tree.empty",
                                            L"（打开或切到一份 XML 文档就会在这里显示结构）"))
                          : ToUtf8(core::Tr(L"json.tree.empty",
                                            L"（打开或切到一份 JSON 文档就会在这里显示结构）"));
        }
        return isXml_
                   ? ToUtf8(core::Tr(L"xml.tree.invalid", L"不是合法的 XML：")) +
                         ToUtf8(xmlTree_.error)
                   : ToUtf8(core::Tr(L"json.tree.invalid", L"不是合法的 JSON：")) +
                         ToUtf8(tree_.error);
    }
    const int first = std::max(topLine_, 0);
    const int last = std::min(static_cast<int>(rows_.size()), first + VisibleLineCount());
    for (int index = first; index < last; ++index) {
        if (!text.empty()) {
            text += "\r\n";
        }
        text += RowText(rows_[static_cast<std::size_t>(index)]);
    }
    const bool truncated = isXml_ ? xmlTree_.truncated : tree_.truncated;
    if (truncated) {
        text += "\r\n";
        text += isXml_
                    ? ToUtf8(core::Tr(L"xml.tree.truncated",
                                      L"（节点太多，只显示了前面一部分）"))
                    : ToUtf8(core::Tr(L"json.tree.truncated",
                                      L"（节点太多，只显示了前面一部分）"));
    }
    return text;
}

void StructureTreePanel::Paint(HDC dc, const RECT& client) {
    const COLORREF background = dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255);
    const COLORREF textColour = dark_ ? RGB(220, 220, 220) : RGB(30, 30, 30);
    const COLORREF nameColour = dark_ ? RGB(156, 220, 254) : RGB(0, 87, 145);
    const COLORREF stringColour = dark_ ? RGB(206, 145, 120) : RGB(163, 21, 21);
    const COLORREF numberColour = dark_ ? RGB(181, 206, 168) : RGB(9, 134, 88);
    const COLORREF dimColour = dark_ ? RGB(150, 150, 150) : RGB(120, 120, 120);

    const HBRUSH backgroundBrush = ::CreateSolidBrush(background);
    ::FillRect(dc, &client, backgroundBrush);
    ::DeleteObject(backgroundBrush);
    ::SetBkMode(dc, TRANSPARENT);

    const HGDIOBJ previousFont = ::SelectObject(dc, font_);
    if (!valid_) {
        ::SetTextColor(dc, dimColour);
        const std::wstring hint = ToWide(VisibleText());
        ::TextOutW(dc, charWidth_, charHeight_, hint.c_str(), static_cast<int>(hint.size()));
        ::SelectObject(dc, previousFont);
        return;
    }

    const Colours colours{textColour, nameColour, stringColour, numberColour, dimColour};
    const std::uint32_t totalRows = static_cast<std::uint32_t>(VisibleLineCount());
    for (std::uint32_t row = 0; row < totalRows; ++row) {
        const std::size_t index = static_cast<std::size_t>(topLine_) + row;
        if (index >= rows_.size()) {
            break;
        }
        const int y = static_cast<int>(row) * charHeight_;
        if (rows_[index].xml != nullptr) {
            PaintRowXml(dc, rows_[index], y, colours);
        } else {
            PaintRowJson(dc, rows_[index], y, colours);
        }
    }
    ::SelectObject(dc, previousFont);
}

int StructureTreePanel::DrawSegment(HDC dc, int x, int y, const std::string& text, COLORREF colour,
                                    HFONT font) const {
    if (text.empty()) {
        return x;
    }
    const HGDIOBJ previous = ::SelectObject(dc, font);
    ::SetTextColor(dc, colour);
    const std::wstring wide = ToWide(text);
    ::TextOutW(dc, x, y, wide.c_str(), static_cast<int>(wide.size()));
    ::SelectObject(dc, previous);
    return x + CellsOf(text) * charWidth_;
}

void StructureTreePanel::PaintRowJson(HDC dc, const Row& row, int y,
                                      const Colours& colours) const {
    const core::JsonNode& node = *row.json;
    int x = 0;
    x = DrawSegment(dc, x, y,
                    std::string(static_cast<std::size_t>(row.depth) * kIndentCells, ' '),
                    colours.dim, font_);
    const bool container = ContainerOf(row);
    x = DrawSegment(dc, x, y, container ? (Collapsed(row) ? "> " : "v ") : "  ", colours.dim,
                    font_);
    if (!node.name.empty()) {
        x = DrawSegment(dc, x, y, "\"" + node.name + "\"", colours.name, boldFont_);
        x = DrawSegment(dc, x, y, ": ", colours.dim, font_);
    }
    if (node.kind == core::JsonKind::Object || node.kind == core::JsonKind::Array) {
        const char open = node.kind == core::JsonKind::Object ? '{' : '[';
        const char close = node.kind == core::JsonKind::Object ? '}' : ']';
        if (Collapsed(row) && node.count > 0) {
            std::string text(1, open);
            text += " " + std::to_string(node.count) + " ";
            text.push_back(close);
            DrawSegment(dc, x, y, text, colours.dim, font_);
        } else {
            DrawSegment(dc, x, y, std::string(1, open), colours.dim, font_);
        }
    } else if (node.kind == core::JsonKind::String) {
        DrawSegment(dc, x, y, "\"" + node.value + "\"", colours.string, font_);
    } else if (node.kind == core::JsonKind::Number) {
        DrawSegment(dc, x, y, node.value, colours.number, font_);
    } else {
        DrawSegment(dc, x, y, node.value, colours.text, font_);
    }
}

void StructureTreePanel::PaintRowXml(HDC dc, const Row& row, int y, const Colours& colours) const {
    const core::XmlNode& node = *row.xml;
    int x = 0;
    x = DrawSegment(dc, x, y,
                    std::string(static_cast<std::size_t>(row.depth) * kIndentCells, ' '),
                    colours.dim, font_);
    const bool container = ContainerOf(row);
    x = DrawSegment(dc, x, y, container ? (Collapsed(row) ? "> " : "v ") : "  ", colours.dim,
                    font_);
    switch (node.kind) {
        case core::XmlNodeKind::Element:
            x = DrawSegment(dc, x, y, "<" + node.name, colours.name, boldFont_);
            if (container && Collapsed(row)) {
                x = DrawSegment(dc, x, y, " ... " + std::to_string(node.children.size()) + " 项",
                                colours.dim, font_);
            }
            DrawSegment(dc, x, y, ">", colours.name, boldFont_);
            break;
        case core::XmlNodeKind::Attribute:
            x = DrawSegment(dc, x, y, "@" + node.name, colours.name, boldFont_);
            x = DrawSegment(dc, x, y, " = ", colours.dim, font_);
            DrawSegment(dc, x, y, "\"" + Ellipsize(node.value) + "\"", colours.string, font_);
            break;
        case core::XmlNodeKind::Text:
            DrawSegment(dc, x, y, "\"" + Ellipsize(node.value) + "\"", colours.string, font_);
            break;
        case core::XmlNodeKind::CData:
            x = DrawSegment(dc, x, y, "CDATA: ", colours.dim, font_);
            DrawSegment(dc, x, y, Ellipsize(node.value), colours.string, font_);
            break;
        case core::XmlNodeKind::Comment:
            DrawSegment(dc, x, y, "<!-- " + Ellipsize(node.value) + " -->", colours.dim, font_);
            break;
        case core::XmlNodeKind::PI:
            x = DrawSegment(dc, x, y, "<?" + node.name, colours.name, boldFont_);
            if (!node.value.empty()) {
                x = DrawSegment(dc, x, y, " " + Ellipsize(node.value), colours.dim, font_);
            }
            DrawSegment(dc, x, y, "?>", colours.dim, font_);
            break;
        case core::XmlNodeKind::Doctype:
            DrawSegment(dc, x, y, Ellipsize(node.value), colours.dim, font_);
            break;
        case core::XmlNodeKind::Document:
            break;   // 文档根不进显示行
    }
}


LRESULT StructureTreePanel::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
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
            UpdateScrollBar();
            return 0;
        case WM_LBUTTONDOWN: {
            if (charHeight_ <= 0) {
                return 0;
            }
            const int y = GET_Y_LPARAM(lParam);
            if (y < 0) {
                return 0;
            }
            const std::size_t index = static_cast<std::size_t>(topLine_) +
                                      static_cast<std::size_t>(y / charHeight_);
            if (index >= rows_.size()) {
                return 0;
            }
            const Row& row = rows_[index];
            if (ContainerOf(row)) {
                Toggle(row);
            } else if (jumpCallback_) {
                // 点了标量 / 属性 / 文本：跳到源码里那一行
                jumpCallback_(row.xml != nullptr ? row.xml->line : row.json->line);
            }
            return 0;
        }
        case WM_VSCROLL: {
            switch (LOWORD(wParam)) {
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
                    ScrollTo(static_cast<int>(rows_.size()));
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
        case WM_KEYDOWN: {
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
                    ScrollTo(static_cast<int>(rows_.size()));
                    return 0;
                default:
                    return 0;
            }
        }
        case WM_GETTEXTLENGTH: {
            const std::wstring text = ToWide(VisibleText());
            return static_cast<LRESULT>(text.size());
        }
        case WM_GETTEXT: {
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

void StructureTreePanel::SetVisible(bool visible) const {
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
}

void StructureTreePanel::Resize(const RECT& area) const {
    if (hwnd_ != nullptr) {
        ::MoveWindow(hwnd_, area.left, area.top, area.right - area.left, area.bottom - area.top,
                     TRUE);
    }
}

void StructureTreePanel::Focus() const {
    if (hwnd_ != nullptr) {
        ::SetFocus(hwnd_);
    }
}

}  // namespace npcc::shell
