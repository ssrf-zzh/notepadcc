#include "core/XPath.h"

#include <windows.h>

#include <cstddef>

namespace npcc::core {
namespace {

std::wstring Widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          need);
    return out;
}

// ---- 表达式解析 ----

enum class PredKind { Index, AttrExists, AttrCompare, ChildExists, ChildCompare, TextCompare };

struct Predicate {
    PredKind     kind = PredKind::Index;
    bool         negate = false;  // != 比较（Compare 类）
    int          index = 0;       // Index
    std::wstring name;            // 属性名 / 子元素名
    std::wstring value;           // 比较值
};

struct Step {
    bool        descendant = false;  // 这一级前面是 //（任意深度）
    bool        attribute = false;   // @属性
    bool        self = false;        // .
    bool        textTest = false;    // text()
    bool        wildcard = false;    // *
    std::wstring name;               // 元素名
    std::vector<Predicate> predicates;
};

struct ParsedPath {
    bool              ok = false;
    std::wstring      error;
    std::vector<Step> steps;
};

bool IsNameStart(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'_' ||
           ch >= 0x80;
}

bool IsNameChar(wchar_t ch) {
    return IsNameStart(ch) || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'.' ||
           ch == L':';
}

// 递归下降的路径解析器。列号按宽字符数（中文元素名也算一列），
// 所有错误都带"表达式第 N 列"前缀，界面直接把这句话显示出来。
class Parser {
public:
    explicit Parser(std::wstring_view text) : text_(text) {}

    ParsedPath Parse() {
        ParsedPath out;
        SkipSpaces();
        if (AtEnd()) {
            out.error = L"表达式为空";
            return out;
        }
        bool descendant = false;
        if (Peek() == L'/') {
            ++pos_;
            if (!AtEnd() && Peek() == L'/') {
                ++pos_;
                descendant = true;
            }
        }
        for (;;) {
            Step step;
            step.descendant = descendant;
            std::wstring error;
            if (!ParseStep(step, error)) {
                out.error = std::move(error);
                return out;
            }
            out.steps.push_back(std::move(step));
            SkipSpaces();
            if (AtEnd()) {
                break;
            }
            if (Peek() != L'/') {
                out.error = At(pos_) + L"只支持一条路径（这里期待 / 或表达式结束）";
                return out;
            }
            ++pos_;
            descendant = false;
            if (!AtEnd() && Peek() == L'/') {
                ++pos_;
                descendant = true;
            }
            SkipSpaces();
            if (AtEnd()) {
                out.error = At(pos_) + L"'/' 后面要跟一个节点测试";
                return out;
            }
        }
        out.ok = true;
        return out;
    }

private:
    bool AtEnd() const { return pos_ >= text_.size(); }

    wchar_t Peek() const { return text_[pos_]; }

    std::wstring At(std::size_t pos) const {
        return L"表达式第 " + std::to_wstring(pos + 1) + L" 列：";
    }

    void SkipSpaces() {
        while (!AtEnd() && (Peek() == L' ' || Peek() == L'\t' || Peek() == L'\r' ||
                            Peek() == L'\n')) {
            ++pos_;
        }
    }

    std::wstring ReadName() {
        const std::size_t begin = pos_;
        while (!AtEnd() && IsNameChar(Peek())) {
            ++pos_;
        }
        return std::wstring(text_.substr(begin, pos_ - begin));
    }

    bool ParseStep(Step& step, std::wstring& error) {
        if (AtEnd()) {
            error = At(pos_) + L"期待节点名、* 或 text()";
            return false;
        }
        const wchar_t ch = Peek();
        if (ch == L'.') {
            ++pos_;
            step.self = true;
        } else if (ch == L'@') {
            ++pos_;
            step.attribute = true;
            if (!AtEnd() && Peek() == L'*') {
                ++pos_;
                step.wildcard = true;
            } else if (!AtEnd() && IsNameStart(Peek())) {
                step.name = ReadName();
            } else {
                error = At(pos_) + L"@ 后面要跟属性名";
                return false;
            }
        } else if (ch == L'*') {
            ++pos_;
            step.wildcard = true;
        } else if (IsNameStart(ch)) {
            std::wstring name = ReadName();
            SkipSpaces();
            if (name == L"text" && !AtEnd() && Peek() == L'(') {
                ++pos_;
                SkipSpaces();
                if (AtEnd() || Peek() != L')') {
                    error = At(pos_) + L"text( 后面要跟 )";
                    return false;
                }
                ++pos_;
                step.textTest = true;
            } else {
                step.name = std::move(name);
            }
        } else {
            error = At(pos_) + L"期待节点名、* 或 text()";
            return false;
        }
        for (;;) {
            SkipSpaces();
            if (AtEnd() || Peek() != L'[') {
                break;
            }
            const std::size_t open = pos_;
            ++pos_;
            Predicate pred;
            if (!ParsePredicate(pred, error)) {
                return false;
            }
            SkipSpaces();
            if (AtEnd() || Peek() != L']') {
                error = At(open) + L"谓词缺一个收尾的 ]";
                return false;
            }
            ++pos_;
            step.predicates.push_back(std::move(pred));
        }
        return true;
    }

    bool ParsePredicate(Predicate& pred, std::wstring& error) {
        SkipSpaces();
        if (AtEnd() || Peek() == L']') {
            error = At(pos_) + L"谓词是空的";
            return false;
        }
        const std::size_t start = pos_;
        if (Peek() >= L'0' && Peek() <= L'9') {
            long long value = 0;
            while (!AtEnd() && Peek() >= L'0' && Peek() <= L'9') {
                value = value * 10 + (Peek() - L'0');
                if (value > 1000000000LL) {
                    error = At(start) + L"序号太大";
                    return false;
                }
                ++pos_;
            }
            pred.kind = PredKind::Index;
            pred.index = static_cast<int>(value);
            return true;
        }
        if (Peek() == L'@') {
            ++pos_;
            SkipSpaces();
            if (AtEnd() || !IsNameStart(Peek())) {
                error = At(pos_) + L"@ 后面要跟属性名";
                return false;
            }
            pred.name = ReadName();
            SkipSpaces();
            if (!AtEnd() && (Peek() == L'=' || Peek() == L'!')) {
                if (!ParseComparison(pred, error)) {
                    return false;
                }
                pred.kind = PredKind::AttrCompare;
            } else {
                pred.kind = PredKind::AttrExists;
            }
            return true;
        }
        if (IsNameStart(Peek())) {
            std::wstring name = ReadName();
            SkipSpaces();
            if (name == L"text" && !AtEnd() && Peek() == L'(') {
                ++pos_;
                SkipSpaces();
                if (AtEnd() || Peek() != L')') {
                    error = At(pos_) + L"text( 后面要跟 )";
                    return false;
                }
                ++pos_;
                SkipSpaces();
                if (AtEnd() || (Peek() != L'=' && Peek() != L'!')) {
                    error = At(pos_) + L"text() 只能这样比：[text()='值']";
                    return false;
                }
                if (!ParseComparison(pred, error)) {
                    return false;
                }
                pred.kind = PredKind::TextCompare;
                return true;
            }
            pred.name = std::move(name);
            if (!AtEnd() && Peek() == L'(') {
                // 形如 [count(*)] 的函数没实现，直接列出能用的写法，
                // 而不是落到"谓词缺一个收尾的 ]"那种对不上号的错误
                error = At(start) +
                        L"只支持 [序号]、[@属性]、[子元素] 和它们的 = / != 比较，以及 [text()='值']";
                return false;
            }
            if (!AtEnd() && (Peek() == L'=' || Peek() == L'!')) {
                if (!ParseComparison(pred, error)) {
                    return false;
                }
                pred.kind = PredKind::ChildCompare;
            } else {
                pred.kind = PredKind::ChildExists;
            }
            return true;
        }
        error = At(start) +
                L"只支持 [序号]、[@属性]、[子元素] 和它们的 = / != 比较，以及 [text()='值']";
        return false;
    }

    bool ParseComparison(Predicate& pred, std::wstring& error) {
        if (!AtEnd() && Peek() == L'!') {
            ++pos_;
            if (AtEnd() || Peek() != L'=') {
                error = At(pos_) + L"只支持 = 与 != 两种比较";
                return false;
            }
            pred.negate = true;
        }
        if (AtEnd() || Peek() != L'=') {
            error = At(pos_) + L"只支持 = 与 != 两种比较";
            return false;
        }
        ++pos_;
        return ReadQuoted(pred.value, error);
    }

    bool ReadQuoted(std::wstring& out, std::wstring& error) {
        SkipSpaces();
        if (AtEnd() || (Peek() != L'\'' && Peek() != L'"')) {
            error = At(pos_) + L"比较值要用引号（'值' 或 \"值\"）括起来";
            return false;
        }
        const wchar_t quote = Peek();
        const std::size_t open = pos_;
        ++pos_;
        const std::size_t begin = pos_;
        while (!AtEnd() && Peek() != quote) {
            ++pos_;
        }
        if (AtEnd()) {
            error = At(open) + L"引号没有闭合";
            return false;
        }
        out.assign(text_.substr(begin, pos_ - begin));
        ++pos_;
        return true;
    }

    std::wstring_view text_;
    std::size_t       pos_ = 0;
};

// ---- 求值 ----

using NodeSet = std::vector<const XmlNode*>;

// 文本内容 = 直接文本 / CDATA 子节点拼接。谓词比较与显示共用这一处口径；
// 元素里嵌套子元素的文字不算（要那种用 [child='值'] 比子元素自己的文本）。
std::wstring TextContent(const XmlNode& node) {
    std::wstring out;
    for (const XmlNode& child : node.children) {
        if (child.kind == XmlNodeKind::Text || child.kind == XmlNodeKind::CData) {
            out += Widen(child.value);
        }
    }
    return out;
}

const XmlNode* FindAttribute(const XmlNode& node, const std::wstring& name) {
    for (const XmlNode& child : node.children) {
        if (child.kind == XmlNodeKind::Attribute && Widen(child.name) == name) {
            return &child;
        }
    }
    return nullptr;
}

const XmlNode* FindChildElement(const XmlNode& node, const std::wstring& name) {
    for (const XmlNode& child : node.children) {
        if (child.kind == XmlNodeKind::Element && Widen(child.name) == name) {
            return &child;
        }
    }
    return nullptr;
}

bool NameMatches(const std::string& name, const Step& step) {
    return step.wildcard || Widen(name) == step.name;
}

bool MatchesPredicate(const XmlNode& node, const Predicate& pred) {
    switch (pred.kind) {
        case PredKind::Index:
            return true;  // 序号在 ApplyPredicates 里单独处理
        case PredKind::AttrExists:
            return FindAttribute(node, pred.name) != nullptr;
        case PredKind::AttrCompare: {
            const XmlNode* attr = FindAttribute(node, pred.name);
            if (attr == nullptr) {
                return false;  // 没有这个属性时 != 也是假（与 XPath 语义一致）
            }
            const bool equal = Widen(attr->value) == pred.value;
            return pred.negate ? !equal : equal;
        }
        case PredKind::ChildExists:
            return FindChildElement(node, pred.name) != nullptr;
        case PredKind::ChildCompare: {
            const XmlNode* child = FindChildElement(node, pred.name);
            if (child == nullptr) {
                return false;
            }
            const bool equal = TextContent(*child) == pred.value;
            return pred.negate ? !equal : equal;
        }
        case PredKind::TextCompare: {
            const bool equal = TextContent(node) == pred.value;
            return pred.negate ? !equal : equal;
        }
    }
    return false;
}

// 谓词按 XPath 语义对这一级选出的"一组"应用（每个上下文节点各自一组），
// 所以 [2] 是"每个父元素的第 2 个子元素"而不是整篇的第 2 个。
void ApplyPredicates(NodeSet& group, const Step& step) {
    for (const Predicate& pred : step.predicates) {
        if (pred.kind == PredKind::Index) {
            if (pred.index >= 1 && static_cast<std::size_t>(pred.index) <= group.size()) {
                const XmlNode* picked = group[static_cast<std::size_t>(pred.index) - 1];
                group.assign(1, picked);
            } else {
                group.clear();
            }
            continue;
        }
        NodeSet kept;
        for (const XmlNode* node : group) {
            if (MatchesPredicate(*node, pred)) {
                kept.push_back(node);
            }
        }
        group.swap(kept);
    }
}

NodeSet SelectGroup(const XmlNode& node, const Step& step) {
    NodeSet group;
    if (step.self) {
        group.push_back(&node);
        ApplyPredicates(group, step);
        return group;
    }
    for (const XmlNode& child : node.children) {
        bool take = false;
        if (step.attribute) {
            take = child.kind == XmlNodeKind::Attribute && NameMatches(child.name, step);
        } else if (step.textTest) {
            take = child.kind == XmlNodeKind::Text || child.kind == XmlNodeKind::CData;
        } else {
            take = child.kind == XmlNodeKind::Element && NameMatches(child.name, step);
        }
        if (take) {
            group.push_back(&child);
        }
    }
    ApplyPredicates(group, step);
    return group;
}

// descendant-or-self 轴：属性节点**不算**后代（XPath 的规矩），
// 所以走子树时跳过属性，避免 //@x 把每个属性算两遍。
template <typename Fn>
void WalkSelfAndDescendants(const XmlNode& node, Fn&& fn) {
    fn(node);
    for (const XmlNode& child : node.children) {
        if (child.kind != XmlNodeKind::Attribute) {
            WalkSelfAndDescendants(child, fn);
        }
    }
}

NodeSet EvaluateSteps(const XmlNode& root, const std::vector<Step>& steps) {
    NodeSet current{&root};
    for (const Step& step : steps) {
        NodeSet next;
        for (const XmlNode* node : current) {
            if (step.descendant) {
                WalkSelfAndDescendants(*node, [&](const XmlNode& walked) {
                    NodeSet group = SelectGroup(walked, step);
                    next.insert(next.end(), group.begin(), group.end());
                });
            } else {
                NodeSet group = SelectGroup(*node, step);
                next.insert(next.end(), group.begin(), group.end());
            }
        }
        current.swap(next);
    }
    return current;
}

std::wstring TruncatePreview(std::wstring text) {
    constexpr std::size_t kPreviewLimit = 60;
    if (text.size() > kPreviewLimit) {
        text.resize(kPreviewLimit);
        text += L'…';
    }
    return text;
}

}  // namespace

std::wstring FormatXPathNodePreview(const XmlNode& node) {
    std::wstring preview;
    switch (node.kind) {
        case XmlNodeKind::Element: {
            preview = L"<" + Widen(node.name);
            for (const XmlNode& child : node.children) {
                if (child.kind == XmlNodeKind::Attribute) {
                    preview += L" " + Widen(child.name) + L"=\"" + Widen(child.value) + L"\"";
                }
            }
            preview += L">";
            break;
        }
        case XmlNodeKind::Attribute:
            preview = L"@" + Widen(node.name) + L"=\"" + Widen(node.value) + L"\"";
            break;
        case XmlNodeKind::Text:
        case XmlNodeKind::CData:
            preview = L"#text=\"" + Widen(node.value) + L"\"";
            break;
        default:
            preview = Widen(node.name);
            break;
    }
    return TruncatePreview(std::move(preview));
}

XPathResult XPathQuery(std::string_view xml, std::wstring_view expression) {
    XPathResult result;
    // 先挑表达式：它是用户刚打进去的东西，报错先报它
    Parser parser(expression);
    ParsedPath path = parser.Parse();
    if (!path.ok) {
        result.error = std::move(path.error);
        return result;
    }
    const XmlTreeResult tree = ParseXmlTree(xml);
    if (!tree.ok) {
        result.error = tree.error;
        return result;
    }
    result.treeTruncated = tree.truncated;

    const NodeSet nodes = EvaluateSteps(tree.root, path.steps);
    for (const XmlNode* node : nodes) {
        XPathMatch match;
        switch (node->kind) {
            case XmlNodeKind::Element:
                match.kind = XPathMatchKind::Element;
                match.name = node->name;
                break;
            case XmlNodeKind::Attribute:
                match.kind = XPathMatchKind::Attribute;
                match.name = node->name;
                match.value = node->value;
                break;
            case XmlNodeKind::Text:
            case XmlNodeKind::CData:
                match.kind = XPathMatchKind::Text;
                match.value = node->value;
                break;
            default:
                continue;  // 文档根 / 注释 / PI / DOCTYPE 不进结果
        }
        match.line = node->line;
        match.preview = FormatXPathNodePreview(*node);
        if (result.matches.size() >= kMaxXPathMatches) {
            result.truncated = true;
            break;
        }
        result.matches.push_back(std::move(match));
    }
    result.ok = true;
    return result;
}

}  // namespace npcc::core
