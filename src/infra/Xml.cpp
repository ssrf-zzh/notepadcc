#include "infra/Xml.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>


namespace npcc::infra {
namespace {

constexpr int kIndentWidth = 2;

std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), need);
    return wide;
}

void AppendUtf8(std::string& out, std::wstring_view text) {
    if (text.empty()) {
        return;
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return;
    }
    const size_t offset = out.size();
    out.resize(offset + static_cast<size_t>(need));
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data() + offset, need, nullptr, nullptr);
}

bool IsNameStart(wchar_t ch) {
    return ::iswalpha(ch) != 0 || ch == L'_' || ch == L':' ||
           (ch >= 0x80);   // 允许非 ASCII 名称，简化处理
}

bool IsNameChar(wchar_t ch) {
    return IsNameStart(ch) || ::iswdigit(ch) != 0 || ch == L'-' || ch == L'.';
}

// ---------------------------------------------------------------- 解析器
class Parser {
public:
    explicit Parser(const std::wstring& source) : src_(source) {}

    bool Run(XmlDocument& out) {
        SkipPrologAndMisc();
        XmlElementPtr root;
        if (!ParseElement(root)) {
            return false;
        }
        // 这里的返回值不能丢：根元素之后还有内容说明文档结构不对
        if (!SkipTrailingMisc()) {
            return false;
        }
        out.SetRoot(std::move(root));
        return true;
    }

    const std::wstring& Error() const { return error_; }

private:
    const std::wstring& src_;
    size_t              pos_ = 0;
    std::wstring        error_;

    bool AtEnd() const { return pos_ >= src_.size(); }
    wchar_t Peek(size_t offset = 0) const {
        const size_t index = pos_ + offset;
        return index < src_.size() ? src_[index] : L'\0';
    }
    bool StartsWith(const wchar_t* token) const {
        const size_t length = std::wcslen(token);
        return src_.compare(pos_, length, token) == 0;
    }
    void SkipWhitespace() {
        while (!AtEnd() && ::iswspace(Peek()) != 0) {
            ++pos_;
        }
    }
    bool Fail(const wchar_t* what) {
        error_ = std::wstring(what) + L"（字符位置 " + std::to_wstring(pos_) + L"）";
        return false;
    }

    bool SkipComment() {
        const size_t end = src_.find(L"-->", pos_ + 4);
        if (end == std::wstring::npos) {
            return Fail(L"注释未闭合");
        }
        pos_ = end + 3;
        return true;
    }

    bool SkipProcessingInstruction() {
        const size_t end = src_.find(L"?>", pos_ + 2);
        if (end == std::wstring::npos) {
            return Fail(L"处理指令未闭合");
        }
        pos_ = end + 2;
        return true;
    }

    bool SkipDoctype() {
        // 只处理最简单的 <!DOCTYPE ...>（不含内部子集），遇到 '[' 就放弃
        size_t index = pos_ + 9;
        int depth = 0;
        while (index < src_.size()) {
            if (src_[index] == L'[') {
                return Fail(L"不支持带内部子集的 DOCTYPE");
            }
            if (src_[index] == L'>' && depth == 0) {
                pos_ = index + 1;
                return true;
            }
            ++index;
        }
        return Fail(L"DOCTYPE 未闭合");
    }

    // 跳过声明、注释、DOCTYPE、空白与处理指令
    bool SkipPrologAndMisc() {
        for (;;) {
            SkipWhitespace();
            if (AtEnd()) {
                return true;
            }
            if (StartsWith(L"<?xml")) {
                if (!SkipProcessingInstruction()) {
                    return false;
                }
                continue;
            }
            if (StartsWith(L"<?")) {
                if (!SkipProcessingInstruction()) {
                    return false;
                }
                continue;
            }
            if (StartsWith(L"<!--")) {
                if (!SkipComment()) {
                    return false;
                }
                continue;
            }
            if (StartsWith(L"<!DOCTYPE")) {
                if (!SkipDoctype()) {
                    return false;
                }
                continue;
            }
            return true;
        }
    }

    bool SkipTrailingMisc() {
        for (;;) {
            SkipWhitespace();
            if (AtEnd()) {
                return true;
            }
            if (StartsWith(L"<!--")) {
                if (!SkipComment()) {
                    return false;
                }
                continue;
            }
            if (StartsWith(L"<?")) {
                if (!SkipProcessingInstruction()) {
                    return false;
                }
                continue;
            }
            return Fail(L"根元素之后出现多余内容");
        }
    }

    bool ReadName(std::wstring& name) {
        if (AtEnd() || !IsNameStart(Peek())) {
            return Fail(L"缺少合法的元素名");
        }
        const size_t start = pos_;
        while (!AtEnd() && IsNameChar(Peek())) {
            ++pos_;
        }
        name = src_.substr(start, pos_ - start);
        return true;
    }

    std::wstring DecodeEntities(std::wstring_view raw) {
        std::wstring out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != L'&') {
                out.push_back(raw[i]);
                continue;
            }
            const size_t semi = raw.find(L';', i + 1);
            if (semi == std::wstring_view::npos || semi - i > 12) {
                out.push_back(L'&');   // 不是实体，按字面量处理
                continue;
            }
            const std::wstring_view name = raw.substr(i + 1, semi - i - 1);
            if (name == L"amp") {
                out.push_back(L'&');
            } else if (name == L"lt") {
                out.push_back(L'<');
            } else if (name == L"gt") {
                out.push_back(L'>');
            } else if (name == L"quot") {
                out.push_back(L'"');
            } else if (name == L"apos") {
                out.push_back(L'\'');
            } else if (!name.empty() && name[0] == L'#') {
                unsigned long code = 0;
                const bool hex = name.size() > 1 && (name[1] == L'x' || name[1] == L'X');
                const std::wstring digits(name.substr(hex ? 2 : 1));
                if (digits.empty()) {
                    out.push_back(L'&');
                    continue;
                }
                wchar_t* end = nullptr;
                code = std::wcstoul(digits.c_str(), &end, hex ? 16 : 10);
                if (end == nullptr || *end != L'\0' || code == 0 || code > 0x10FFFF) {
                    out.push_back(L'&');
                    continue;
                }
                // 仅处理 BMP 内码点；超出范围的字符本项目不会用到
                if (code <= 0xFFFF) {
                    out.push_back(static_cast<wchar_t>(code));
                }
            } else {
                out.push_back(L'&');
                continue;
            }
            i = semi;
        }
        return out;
    }

    bool ReadQuoted(std::wstring& value) {
        const wchar_t quote = Peek();
        if (quote != L'"' && quote != L'\'') {
            return Fail(L"属性值缺少引号");
        }
        ++pos_;
        const size_t start = pos_;
        while (!AtEnd() && Peek() != quote) {
            ++pos_;
        }
        if (AtEnd()) {
            return Fail(L"属性值未闭合");
        }
        value = DecodeEntities(src_.substr(start, pos_ - start));
        ++pos_;   // 跳过收尾引号
        return true;
    }

    bool ParseAttributes(XmlElement& element) {
        for (;;) {
            SkipWhitespace();
            const wchar_t ch = Peek();
            if (ch == L'>' || (ch == L'/' && Peek(1) == L'>')) {
                return true;
            }
            if (AtEnd()) {
                return Fail(L"标签未闭合");
            }
            XmlAttribute attribute;
            if (!ReadName(attribute.name)) {
                return false;
            }
            SkipWhitespace();
            if (Peek() != L'=') {
                return Fail(L"属性缺少等号");
            }
            ++pos_;
            SkipWhitespace();
            if (!ReadQuoted(attribute.value)) {
                return false;
            }
            element.attributes.push_back(std::move(attribute));
        }
    }

    bool ParseElement(XmlElementPtr& out) {
        if (Peek() != L'<') {
            return Fail(L"缺少起始标签");
        }
        ++pos_;
        auto element = std::make_shared<XmlElement>();
        if (!ReadName(element->name)) {
            return false;
        }
        if (!ParseAttributes(*element)) {
            return false;
        }
        if (Peek() == L'/' && Peek(1) == L'>') {
            pos_ += 2;
            out = std::move(element);
            return true;
        }
        ++pos_;   // 跳过 '>'

        std::wstring text;
        for (;;) {
            if (AtEnd()) {
                return Fail(L"元素未闭合");
            }
            if (StartsWith(L"</")) {
                pos_ += 2;
                std::wstring closing;
                if (!ReadName(closing)) {
                    return false;
                }
                SkipWhitespace();
                if (Peek() != L'>') {
                    return Fail(L"结束标签格式错误");
                }
                ++pos_;
                if (closing != element->name) {
                    return Fail(L"结束标签与起始标签不匹配");
                }
                break;
            }
            if (StartsWith(L"<!--")) {
                if (!SkipComment()) {
                    return false;
                }
                continue;
            }
            if (StartsWith(L"<![CDATA[")) {
                const size_t end = src_.find(L"]]>", pos_ + 9);
                if (end == std::wstring::npos) {
                    return Fail(L"CDATA 未闭合");
                }
                text.append(src_, pos_ + 9, end - (pos_ + 9));
                pos_ = end + 3;
                continue;
            }
            if (StartsWith(L"<?")) {
                if (!SkipProcessingInstruction()) {
                    return false;
                }
                continue;
            }
            if (Peek() == L'<') {
                XmlElementPtr child;
                if (!ParseElement(child)) {
                    return false;
                }
                element->children.push_back(std::move(child));
                continue;
            }
            const size_t start = pos_;
            while (!AtEnd() && Peek() != L'<') {
                ++pos_;
            }
            text += DecodeEntities(src_.substr(start, pos_ - start));
        }
        element->text = std::move(text);
        out = std::move(element);
        return true;
    }
};

// ---------------------------------------------------------------- 序列化
void AppendEscaped(std::string& out, std::wstring_view text, bool inAttribute) {
    std::wstring buffer;
    buffer.reserve(text.size());
    for (const wchar_t ch : text) {
        switch (ch) {
            case L'&':  buffer += L"&amp;"; break;
            case L'<':  buffer += L"&lt;"; break;
            case L'>':  buffer += L"&gt;"; break;
            case L'"':  buffer += inAttribute ? L"&quot;" : L"\""; break;
            case L'\'': buffer += inAttribute ? L"&apos;" : L"'"; break;
            case L'\n': buffer += inAttribute ? L"&#10;" : L"\n"; break;
            case L'\r': buffer += inAttribute ? L"&#13;" : L"\r"; break;
            case L'\t': buffer += inAttribute ? L"&#9;" : L"\t"; break;
            default:    buffer.push_back(ch); break;
        }
    }
    AppendUtf8(out, buffer);
}

void AppendIndent(std::string& out, int depth) {
    out.append(static_cast<size_t>(depth * kIndentWidth), ' ');
}

void WriteElement(std::string& out, const XmlElement& element, int depth) {
    AppendIndent(out, depth);
    out += '<';
    AppendUtf8(out, element.name);
    for (const XmlAttribute& attribute : element.attributes) {
        out += ' ';
        AppendUtf8(out, attribute.name);
        out += "=\"";
        AppendEscaped(out, attribute.value, true);
        out += '"';
    }

    const bool hasText = !element.text.empty();
    if (element.children.empty() && !hasText) {
        out += "/>\r\n";
        return;
    }
    out += '>';

    if (element.children.empty()) {
        AppendEscaped(out, element.text, false);
        out += "</";
        AppendUtf8(out, element.name);
        out += ">\r\n";
        return;
    }

    if (hasText) {
        AppendEscaped(out, element.text, false);
    }
    out += "\r\n";
    for (const XmlElementPtr& child : element.children) {
        if (child) {
            WriteElement(out, *child, depth + 1);
        }
    }
    AppendIndent(out, depth);
    out += "</";
    AppendUtf8(out, element.name);
    out += ">\r\n";
}

}  // namespace

const std::wstring* XmlElement::Attribute(std::wstring_view key) const {
    for (const XmlAttribute& attribute : attributes) {
        if (attribute.name == key) {
            return &attribute.value;
        }
    }
    return nullptr;
}

std::wstring XmlElement::AttributeOr(std::wstring_view key, std::wstring_view fallback) const {
    const std::wstring* value = Attribute(key);
    return value != nullptr ? *value : std::wstring(fallback);
}

int XmlElement::AttributeInt(std::wstring_view key, int fallback) const {
    const std::wstring* value = Attribute(key);
    if (value == nullptr || value->empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value->c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') {
        return fallback;
    }
    return static_cast<int>(parsed);
}

bool XmlElement::AttributeBool(std::wstring_view key, bool fallback) const {
    const std::wstring* value = Attribute(key);
    if (value == nullptr) {
        return fallback;
    }
    if (*value == L"yes" || *value == L"true" || *value == L"1") {
        return true;
    }
    if (*value == L"no" || *value == L"false" || *value == L"0") {
        return false;
    }
    return fallback;
}

void XmlElement::SetAttribute(std::wstring_view key, std::wstring_view value) {
    for (XmlAttribute& attribute : attributes) {
        if (attribute.name == key) {
            attribute.value = value;
            return;
        }
    }
    attributes.push_back(XmlAttribute{std::wstring(key), std::wstring(value)});
}

void XmlElement::SetAttributeInt(std::wstring_view key, int value) {
    SetAttribute(key, std::to_wstring(value));
}

void XmlElement::SetAttributeUInt(std::wstring_view key, unsigned int value) {
    SetAttribute(key, std::to_wstring(value));
}

void XmlElement::SetAttributeBool(std::wstring_view key, bool value) {
    SetAttribute(key, std::wstring_view(value ? L"yes" : L"no"));
}

const XmlElement* XmlElement::Child(std::wstring_view childName) const {
    for (const XmlElementPtr& child : children) {
        if (child && child->name == childName) {
            return child.get();
        }
    }
    return nullptr;
}

std::vector<const XmlElement*> XmlElement::Children(std::wstring_view childName) const {
    std::vector<const XmlElement*> result;
    for (const XmlElementPtr& child : children) {
        if (child && child->name == childName) {
            result.push_back(child.get());
        }
    }
    return result;
}

XmlElementPtr XmlElement::AddChild(std::wstring_view childName) {
    auto child = std::make_shared<XmlElement>();
    child->name = std::wstring(childName);
    children.push_back(child);
    return child;
}

bool XmlDocument::Parse(std::string_view utf8, XmlDocument& out, std::wstring* error) {
    std::wstring text = ToWide(utf8);
    if (text.empty() && !utf8.empty()) {
        if (error != nullptr) {
            *error = L"XML 内容不是合法的 UTF-8";
        }
        return false;
    }
    // 去 BOM
    if (!text.empty() && text[0] == 0xFEFF) {
        text.erase(0, 1);
    }
    if (text.empty()) {
        if (error != nullptr) {
            *error = L"XML 内容为空";
        }
        return false;
    }

    Parser parser(text);
    XmlDocument parsed;
    if (!parser.Run(parsed)) {
        if (error != nullptr) {
            *error = parser.Error();
        }
        return false;
    }
    out = std::move(parsed);
    return true;
}

std::string XmlDocument::ToUtf8() const {
    std::string out;
    out.reserve(1024);
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    if (root_) {
        WriteElement(out, *root_, 0);
    }
    return out;
}

XmlElementPtr XmlDocument::CreateRoot(std::wstring_view rootName) {
    root_ = std::make_shared<XmlElement>();
    root_->name = std::wstring(rootName);
    return root_;
}

}  // namespace npcc::infra
