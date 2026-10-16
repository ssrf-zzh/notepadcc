#include "core/XmlTree.h"

#include <algorithm>
#include <cctype>

#include "core/XmlFormat.h"

namespace npcc::core {
namespace {

bool IsSpaceChar(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool IsNameStart(unsigned char ch) {
    return std::isalpha(ch) != 0 || ch == '_' || ch == ':' || ch >= 0x80;
}

bool IsNameBody(unsigned char ch) {
    return IsNameStart(ch) || std::isdigit(ch) != 0 || ch == '-' || ch == '.';
}

bool AllSpace(std::string_view text) {
    for (char ch : text) {
        if (!IsSpaceChar(ch)) {
            return false;
        }
    }
    return true;
}

std::string_view TrimSpace(std::string_view text) {
    while (!text.empty() && IsSpaceChar(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpaceChar(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// 在"已经确认合法"的 XML 上走一遍（合法性由 FormatXml 保证，这里不做错误处理），
// 只负责把结构与行号记下来。推进统一走 AdvanceTo——它顺手数换行，
// line_ 因此永远是 index_ 所在的行（属性值里换行也不会把行号带偏）。
class TreeReader {
public:
    TreeReader(std::string_view text, XmlTreeResult& result)
        : text_(text), result_(result) {
        result_.root.kind = XmlNodeKind::Document;
    }

    void Run() {
        stack_.push_back(&result_.root);
        while (index_ < text_.size() && !result_.truncated) {
            if (text_[index_] != '<') {
                ReadText();
            } else if (StartsWith("<!--")) {
                ReadComment();
            } else if (StartsWith("<![CDATA[")) {
                ReadCData();
            } else if (StartsWith("<?")) {
                ReadPI();
            } else if (StartsWith("<!DOCTYPE") || StartsWith("<!doctype")) {
                ReadDoctype();
            } else if (StartsWith("</")) {
                ReadClose();
            } else {
                ReadOpen();
            }
        }
    }

private:
    bool AtEnd() const { return index_ >= text_.size(); }

    bool StartsWith(std::string_view prefix) const {
        return text_.compare(index_, prefix.size(), prefix) == 0;
    }

    void AdvanceTo(std::size_t to) {
        while (index_ < to && index_ < text_.size()) {
            if (text_[index_] == '\n') {
                ++line_;
            }
            ++index_;
        }
    }

    // 建一个节点挂到 parent 下；到上限就置 truncated 并返回空（调用方据此停下来）
    XmlNode* Append(XmlNode& parent, XmlNodeKind kind, std::string_view name,
                    std::string_view value, int line) {
        if (result_.nodeCount >= kMaxXmlNodes) {
            result_.truncated = true;
            return nullptr;
        }
        XmlNode node;
        node.kind = kind;
        node.name.assign(name.data(), name.size());
        node.value.assign(value.data(), value.size());
        node.line = line;
        parent.children.push_back(std::move(node));
        ++result_.nodeCount;
        return &parent.children.back();
    }

    void ReadText() {
        const int startLine = line_;
        const std::size_t start = index_;
        std::size_t next = text_.find('<', index_);
        if (next == std::string_view::npos) {
            next = text_.size();
        }
        AdvanceTo(next);
        const std::string_view raw = text_.substr(start, next - start);
        if (!AllSpace(raw)) {
            Append(*stack_.back(), XmlNodeKind::Text, {}, raw, startLine);
        }
    }

    void ReadComment() {
        const int startLine = line_;
        const std::size_t start = index_ + 4;
        std::size_t close = text_.find("-->", start);
        if (close == std::string_view::npos) {
            close = text_.size();
        }
        const std::string_view content = text_.substr(start, close - start);
        Append(*stack_.back(), XmlNodeKind::Comment, {}, content, startLine);
        AdvanceTo(close + 3);
    }

    void ReadCData() {
        const int startLine = line_;
        const std::size_t start = index_ + 9;
        std::size_t close = text_.find("]]>", start);
        if (close == std::string_view::npos) {
            close = text_.size();
        }
        const std::string_view content = text_.substr(start, close - start);
        Append(*stack_.back(), XmlNodeKind::CData, {}, content, startLine);
        AdvanceTo(close + 3);
    }

    void ReadPI() {
        const int startLine = line_;
        std::size_t cursor = index_ + 2;
        while (cursor < text_.size() && IsNameBody(static_cast<unsigned char>(text_[cursor]))) {
            ++cursor;
        }
        const std::string_view target = text_.substr(index_ + 2, cursor - (index_ + 2));
        std::size_t close = text_.find("?>", cursor);
        if (close == std::string_view::npos) {
            close = text_.size();
        }
        const std::string_view rest = TrimSpace(text_.substr(cursor, close - cursor));
        Append(*stack_.back(), XmlNodeKind::PI, target, rest, startLine);
        AdvanceTo(close + 2);
    }

    void ReadDoctype() {
        const int startLine = line_;
        const std::size_t start = index_;
        // 内部子集 `[ ... ]` 里也会有 `>`，要一起吃掉（与 XmlFormat 同一套判断）
        std::size_t cursor = index_ + 9;
        int subsetDepth = 0;
        while (cursor < text_.size()) {
            const char ch = text_[cursor];
            if (ch == '[') {
                ++subsetDepth;
            } else if (ch == ']') {
                subsetDepth = std::max(0, subsetDepth - 1);
            } else if (ch == '>' && subsetDepth == 0) {
                break;
            }
            ++cursor;
        }
        const std::size_t end = cursor < text_.size() ? cursor + 1 : text_.size();
        Append(*stack_.back(), XmlNodeKind::Doctype, {}, text_.substr(start, end - start),
               startLine);
        AdvanceTo(end);
    }

    void ReadClose() {
        std::size_t cursor = index_ + 2;
        while (cursor < text_.size() && text_[cursor] != '>') {
            ++cursor;
        }
        AdvanceTo(cursor < text_.size() ? cursor + 1 : text_.size());
        if (stack_.size() > 1) {
            stack_.pop_back();   // 配对由 FormatXml 保证，这里只对栈
        }
    }

    void ReadOpen() {
        const int startLine = line_;
        std::size_t cursor = index_ + 1;
        const std::size_t nameBegin = cursor;
        while (cursor < text_.size() && IsNameBody(static_cast<unsigned char>(text_[cursor]))) {
            ++cursor;
        }
        const std::string_view name = text_.substr(nameBegin, cursor - nameBegin);
        AdvanceTo(cursor);
        XmlNode* element = Append(*stack_.back(), XmlNodeKind::Element, name, {}, startLine);
        if (element == nullptr) {
            return;   // 到上限，外层循环会停
        }
        bool selfClosing = false;
        // 属性列表：[属性名] 空白 '=' 空白 引号 值 引号 …… 直到 '>'（或 '/>'）
        while (!AtEnd()) {
            std::size_t p = index_;
            while (p < text_.size() && IsSpaceChar(text_[p])) {
                ++p;
            }
            AdvanceTo(p);
            if (AtEnd()) {
                break;   // 不该发生：FormatXml 已保证有收尾
            }
            if (text_[index_] == '>') {
                AdvanceTo(index_ + 1);
                break;
            }
            if (text_[index_] == '/') {
                selfClosing = true;
                AdvanceTo(index_ + 1);
                if (!AtEnd() && text_[index_] == '>') {
                    AdvanceTo(index_ + 1);
                }
                break;
            }
            const int attrLine = line_;
            p = index_;
            while (p < text_.size() && IsNameBody(static_cast<unsigned char>(text_[p]))) {
                ++p;
            }
            if (p == index_) {
                AdvanceTo(index_ + 1);   // 防死循环：合法输入不该出现，出现了就跳过
                continue;
            }
            const std::string_view attrName = text_.substr(index_, p - index_);
            AdvanceTo(p);
            p = index_;
            while (p < text_.size() && IsSpaceChar(text_[p])) {
                ++p;
            }
            AdvanceTo(p);
            if (!AtEnd() && text_[index_] == '=') {
                AdvanceTo(index_ + 1);
            }
            p = index_;
            while (p < text_.size() && IsSpaceChar(text_[p])) {
                ++p;
            }
            AdvanceTo(p);
            std::string_view attrValue;
            if (!AtEnd() && (text_[index_] == '"' || text_[index_] == '\'')) {
                const char quote = text_[index_];
                AdvanceTo(index_ + 1);
                const std::size_t valueBegin = index_;
                while (!AtEnd() && text_[index_] != quote) {
                    AdvanceTo(index_ + 1);   // 值里可能有换行，line_ 跟着走
                }
                attrValue = text_.substr(valueBegin, index_ - valueBegin);
                if (!AtEnd()) {
                    AdvanceTo(index_ + 1);   // 吃掉闭合引号
                }
            }
            Append(*element, XmlNodeKind::Attribute, attrName, attrValue, attrLine);
        }
        if (!selfClosing) {
            stack_.push_back(element);
        }
    }

    std::string_view      text_;
    XmlTreeResult&        result_;
    std::size_t           index_ = 0;
    int                   line_ = 1;
    std::vector<XmlNode*> stack_;
};

}  // namespace

XmlTreeResult ParseXmlTree(std::string_view text) {
    XmlTreeResult result;
    // 合法性的唯一来源是 XmlFormat：过不了它这一关，树也不该建
    const FormatResult check = FormatXml(text);
    if (!check.ok) {
        result.error = check.error;
        return result;
    }
    TreeReader(text, result).Run();
    result.ok = true;
    return result;
}

}  // namespace npcc::core
