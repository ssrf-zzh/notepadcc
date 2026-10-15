#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "search/RegexEngine.h"
#include "search/SearchTypes.h"

namespace npcc::edit {
class EditView;
}

namespace npcc::search {

struct SearchConfig {
    std::wstring  pattern;
    std::wstring  replaceTo;
    SearchMode    mode = SearchMode::Literal;
    bool          matchCase = false;
    bool          wholeWordOnly = false;
    bool          dotMatchesNewline = false;
    bool          wrapAround = true;
    bool          inSelectionOnly = false;

    // 由调用方按文档编码设置：UTF-8 文档置 utfMode，ANSI 文档给 codePage
    bool          utfMode = true;
    std::uint32_t codePage = 936;
};

// 单文档的查找与替换（设计文档 2.5、5.4）。
//
// 本类不弹任何对话框：需要用户决策的地方通过返回值上报，由外壳决定怎么问。
// 字面量/扩展模式走 Scintilla 原生搜索（最快，且大小写折叠由 Scintilla 处理）；
// 正则模式走 PCRE2。
class InDocSearch {
public:
    // 单次查找的最多匹配数（FindNext 前向路径不用它）
    static constexpr std::size_t kMaxCollectMatches = 20000;
    static constexpr std::size_t kMaxReplacements = 200000;

    explicit InDocSearch(edit::EditView& view);
    ~InDocSearch();
    InDocSearch(const InDocSearch&) = delete;
    InDocSearch& operator=(const InDocSearch&) = delete;

    // 从当前选区出发查找。返回 false 且 outcome.Ok() 表示"没有更多匹配"。
    bool FindNext(const SearchConfig& config, bool forward, bool& wrapped,
                  SearchOutcome& outcome);

    bool Count(const SearchConfig& config, int& count, SearchOutcome& outcome);
    bool MarkAll(const SearchConfig& config, int& count, SearchOutcome& outcome);
    void ClearMarks() const;

    // 替换当前匹配（若选区不是匹配项，则先找到下一个再替换）
    bool ReplaceCurrent(const SearchConfig& config, bool& replaced, SearchOutcome& outcome);
    // 全部替换；整个过程包在单个撤销单元里
    bool ReplaceAll(const SearchConfig& config, int& count, SearchOutcome& outcome);

private:
    bool Prepare(const SearchConfig& config, SearchOutcome& outcome);
    bool CollectMatches(const SearchConfig& config, std::vector<MatchRange>& matches,
                        std::size_t limit, bool& truncated, SearchOutcome& outcome);
    bool CollectLiteralMatches(const SearchConfig& config, std::int64_t from, std::int64_t to,
                               std::vector<MatchRange>& matches, std::size_t limit,
                               bool& truncated) const;
    bool CollectRegexMatches(std::string_view subject, std::vector<MatchRange>& matches,
                             std::size_t limit, bool& truncated, SearchOutcome& outcome);

    edit::EditView&              view_;
    std::string                  patternBytes_;   // Literal / Escaped 模式
    std::unique_ptr<RegexEngine> regex_;
};

}  // namespace npcc::search
