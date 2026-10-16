#include "core/JsonTree.h"

#include <cctype>

#include "core/JsonFormat.h"

namespace npcc::core {
namespace {

// 在"已经确认合法"的 JSON 上走一遍。这里的扫描不做错误处理（错了也不该发生），
// 只负责把结构与行号记下来——所以比 JsonFormat 里那个递归下降短得多
class TreeReader {
public:
    TreeReader(std::string_view text, bool* truncated, int* nodeCount)
        : text_(text), truncated_(truncated), nodeCount_(nodeCount) {}

    JsonNode ReadDocument() {
        SkipSpace();
        JsonNode root = ReadValue();
        return root;
    }

private:
    bool AtEnd() const { return index_ >= text_.size(); }
    char Peek() const { return AtEnd() ? '\0' : text_[index_]; }

    void Advance(std::size_t count = 1) {
        for (std::size_t step = 0; step < count && !AtEnd(); ++step) {
            if (text_[index_] == '\n') {
                ++line_;
            }
            ++index_;
        }
    }

    void SkipSpace() {
        while (!AtEnd()) {
            const char ch = text_[index_];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                Advance();
                continue;
            }
            break;
        }
    }

    // 返回 false = 到上限了，不再往下建（调用方据此收尾）
    bool BudgetExhausted() {
        if (*nodeCount_ >= kMaxJsonNodes) {
            *truncated_ = true;
            return true;
        }
        ++(*nodeCount_);
        return false;
    }

    std::string ReadStringRaw() {
        std::string out;
        Advance();   // 开引号
        while (!AtEnd()) {
            const char ch = text_[index_];
            if (ch == '\\' && index_ + 1 < text_.size()) {
                out.push_back(ch);
                out.push_back(text_[index_ + 1]);
                Advance(2);   // 转义原样收下（连长度都不解释）
                continue;
            }
            if (ch == '"') {
                Advance();
                break;
            }
            out.push_back(ch);
            Advance();
        }
        return out;
    }

    std::string ReadBareToken() {
        std::string out;
        while (!AtEnd()) {
            const char ch = text_[index_];
            if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
                ch == '\r' || ch == '\n') {
                break;
            }
            out.push_back(ch);
            Advance();
        }
        return out;
    }

    JsonNode ReadValue() {
        JsonNode node;
        node.line = line_;
        if (BudgetExhausted()) {
            return node;   // 到上限了：返回一个空节点，调用方会停下来
        }
        const char ch = Peek();
        if (ch == '{') {
            node.kind = JsonKind::Object;
            Advance();
            SkipSpace();
            if (Peek() == '}') {
                Advance();
                node.count = 0;
                return node;
            }
            for (;;) {
                if (*truncated_) {
                    return node;
                }
                SkipSpace();
                std::string name;
                if (Peek() == '"') {
                    name = ReadStringRaw();
                }
                SkipSpace();
                if (Peek() == ':') {
                    Advance();
                }
                SkipSpace();
                JsonNode child = ReadValue();
                if (*truncated_) {
                    break;   // 预算用完了：这个节点不算数，别留下一个空壳
                }
                child.name = std::move(name);
                node.children.push_back(std::move(child));
                SkipSpace();
                if (Peek() == ',') {
                    Advance();
                    continue;
                }
                if (Peek() == '}') {
                    Advance();
                }
                break;
            }
            node.count = static_cast<int>(node.children.size());
            return node;
        }
        if (ch == '[') {
            node.kind = JsonKind::Array;
            Advance();
            SkipSpace();
            if (Peek() == ']') {
                Advance();
                node.count = 0;
                return node;
            }
            for (;;) {
                if (*truncated_) {
                    return node;
                }
                SkipSpace();
                JsonNode child = ReadValue();
                if (*truncated_) {
                    break;
                }
                node.children.push_back(std::move(child));
                SkipSpace();
                if (Peek() == ',') {
                    Advance();
                    continue;
                }
                if (Peek() == ']') {
                    Advance();
                }
                break;
            }
            node.count = static_cast<int>(node.children.size());
            return node;
        }
        if (ch == '"') {
            node.kind = JsonKind::String;
            node.value = ReadStringRaw();
            return node;
        }
        const std::string token = ReadBareToken();
        node.value = token;
        if (token == "true" || token == "false") {
            node.kind = JsonKind::Bool;
        } else if (token == "null") {
            node.kind = JsonKind::Null;
        } else {
            node.kind = JsonKind::Number;
        }
        return node;
    }

    std::string_view text_;
    std::size_t      index_ = 0;
    int              line_ = 1;
    bool*            truncated_ = nullptr;
    int*             nodeCount_ = nullptr;
};

}  // namespace

JsonTreeResult ParseJsonTree(std::string_view text) {
    JsonTreeResult result;
    // 第一步：校验。错误信息（含"第 N 行第 M 列"）直接来自格式化器，两边口径一致
    const FormatResult checked = FormatJson(text);
    if (!checked.ok) {
        result.error = checked.error;
        return result;
    }
    TreeReader reader(text, &result.truncated, &result.nodeCount);
    result.root = reader.ReadDocument();
    result.ok = true;
    return result;
}

}  // namespace npcc::core
