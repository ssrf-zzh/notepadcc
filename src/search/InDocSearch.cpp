#include "search/InDocSearch.h"

#include <algorithm>

#include "edit/EditView.h"
#include "search/EscapeSequence.h"
#include "search/TextBytes.h"

namespace npcc::search {

InDocSearch::InDocSearch(edit::EditView& view) : view_(view) {}
InDocSearch::~InDocSearch() = default;

bool InDocSearch::Prepare(const SearchConfig& config, SearchOutcome& outcome) {
    outcome = SearchOutcome::Success();
    patternBytes_.clear();
    regex_.reset();

    if (config.pattern.empty()) {
        outcome = SearchOutcome::Failure(SearchError::EmptyPattern, L"搜索内容为空。");
        return false;
    }

    std::string raw;
    if (!ConvertToDocBytes(config.pattern, config.utfMode, config.codePage, raw)) {
        outcome = SearchOutcome::Failure(SearchError::EncodingFailed,
                                         L"搜索内容无法按当前文档编码表示。");
        return false;
    }

    switch (config.mode) {
        case SearchMode::Literal:
            patternBytes_ = std::move(raw);
            break;
        case SearchMode::Escaped:
            if (!UnescapeExtended(raw, patternBytes_, outcome)) {
                return false;
            }
            break;
        case SearchMode::Regex: {
            RegexOptions options;
            options.utfMode = config.utfMode;
            options.codePage = config.codePage;
            options.matchCase = config.matchCase;
            options.dotMatchesNewline = config.dotMatchesNewline;
            options.wholeWordOnly = config.wholeWordOnly;
            regex_ = std::make_unique<RegexEngine>();
            if (!regex_->Compile(config.pattern, options, outcome)) {
                regex_.reset();
                return false;
            }
            return true;
        }
    }

    if (patternBytes_.empty()) {
        outcome = SearchOutcome::Failure(SearchError::EmptyPattern, L"搜索内容为空。");
        return false;
    }
    return true;
}

bool InDocSearch::CollectLiteralMatches(const SearchConfig& config, std::int64_t from,
                                        std::int64_t to, std::vector<MatchRange>& matches,
                                        std::size_t limit, bool& truncated) const {
    std::int32_t flags = 0;
    if (config.matchCase) {
        flags |= edit::EditView::kSearchFlagMatchCase;
    }
    if (config.wholeWordOnly) {
        flags |= edit::EditView::kSearchFlagWholeWord;
    }
    view_.SetSearchFlags(flags);

    const std::int64_t patternLength = static_cast<std::int64_t>(patternBytes_.size());
    const std::int64_t step = std::max<std::int64_t>(1, patternLength);
    std::int64_t position = from;
    while (position <= to) {
        if (matches.size() >= limit) {
            truncated = true;
            break;
        }
        view_.SetTarget(position, to);
        const std::int64_t found = view_.SearchInTarget(patternBytes_);
        if (found < 0) {
            break;
        }
        matches.push_back(MatchRange{found, found + patternLength});
        position = found + step;
    }
    return true;
}

bool InDocSearch::CollectRegexMatches(std::string_view subject, std::vector<MatchRange>& matches,
                                      std::size_t limit, bool& truncated,
                                      SearchOutcome& outcome) {
    if (regex_ == nullptr) {
        outcome = SearchOutcome::Failure(SearchError::InternalError, L"正则尚未编译。");
        return false;
    }
    std::int64_t from = 0;
    const std::int64_t length = static_cast<std::int64_t>(subject.size());
    while (from <= length) {
        if (matches.size() >= limit) {
            truncated = true;
            break;
        }
        MatchRange match;
        if (!regex_->Find(subject, from, match, outcome)) {
            // outcome 有错说明是真失败；否则只是没有更多匹配
            return outcome.Ok();
        }
        matches.push_back(match);
        // 空匹配必须强制前进，否则会原地死循环
        from = (match.end > match.start) ? match.end : match.start + 1;
    }
    return true;
}

bool InDocSearch::CollectMatches(const SearchConfig& config, std::vector<MatchRange>& matches,
                                 std::size_t limit, bool& truncated, SearchOutcome& outcome) {
    matches.clear();
    truncated = false;

    const std::int64_t length = view_.TextLength();
    std::int64_t begin = 0;
    std::int64_t end = length;
    if (config.inSelectionOnly) {
        begin = view_.SelectionStart();
        end = view_.SelectionEnd();
        if (end <= begin) {
            return true;   // 空选区里什么都搜不到
        }
    }

    if (config.mode == SearchMode::Regex) {
        const std::string subject = view_.GetAllText();
        if (!CollectRegexMatches(subject, matches, limit, truncated, outcome)) {
            return false;
        }
        if (config.inSelectionOnly) {
            matches.erase(std::remove_if(matches.begin(), matches.end(),
                                         [begin, end](const MatchRange& match) {
                                             return match.start < begin || match.end > end;
                                         }),
                          matches.end());
        }
        return true;
    }
    return CollectLiteralMatches(config, begin, end, matches, limit, truncated);
}

bool InDocSearch::FindNext(const SearchConfig& config, bool forward, bool& wrapped,
                           SearchOutcome& outcome) {
    wrapped = false;
    if (!Prepare(config, outcome)) {
        return false;
    }

    const std::int64_t length = view_.TextLength();
    const std::int64_t anchorStart = view_.SelectionStart();
    const std::int64_t anchorEnd = view_.SelectionEnd();
    const bool hasSelection = anchorEnd > anchorStart;

    // 命中后选中并滚动到可见
    auto accept = [this](const MatchRange& match) {
        view_.SetSelectionRange(match.start, match.end);
        view_.ScrollCaretToVisible();
    };

    if (config.mode != SearchMode::Regex) {
        std::int32_t flags = 0;
        if (config.matchCase) {
            flags |= edit::EditView::kSearchFlagMatchCase;
        }
        if (config.wholeWordOnly) {
            flags |= edit::EditView::kSearchFlagWholeWord;
        }
        view_.SetSearchFlags(flags);

        const std::int64_t patternLength = static_cast<std::int64_t>(patternBytes_.size());
        const std::int64_t step = std::max<std::int64_t>(1, patternLength);

        if (forward) {
            const std::int64_t start = hasSelection ? anchorEnd : anchorStart;
            view_.SetTarget(start, length);
            const std::int64_t found = view_.SearchInTarget(patternBytes_);
            if (found >= 0) {
                accept(MatchRange{found, found + patternLength});
                return true;
            }
            if (config.wrapAround) {
                view_.SetTarget(0, length);
                const std::int64_t wrapFound = view_.SearchInTarget(patternBytes_);
                if (wrapFound >= 0) {
                    wrapped = true;
                    accept(MatchRange{wrapFound, wrapFound + patternLength});
                    return true;
                }
            }
            return false;
        }

        // 后向：Scintilla 只支持向前搜，所以从文档头扫到锚点，留住最后一个
        const std::int64_t limit = hasSelection ? anchorStart : anchorStart;
        std::int64_t position = 0;
        std::int64_t best = -1;
        while (position <= limit) {
            view_.SetTarget(position, limit);
            const std::int64_t found = view_.SearchInTarget(patternBytes_);
            if (found < 0) {
                break;
            }
            best = found;
            position = found + step;
        }
        if (best >= 0) {
            accept(MatchRange{best, best + patternLength});
            return true;
        }
        if (config.wrapAround) {
            std::int64_t last = -1;
            position = limit;
            while (position <= length) {
                view_.SetTarget(position, length);
                const std::int64_t found = view_.SearchInTarget(patternBytes_);
                if (found < 0) {
                    break;
                }
                last = found;
                position = found + step;
            }
            if (last >= 0) {
                wrapped = true;
                accept(MatchRange{last, last + patternLength});
                return true;
            }
        }
        return false;
    }

    // 正则：拿到整篇文本后定位
    const std::string subject = view_.GetAllText();
    const std::int64_t subjectLength = static_cast<std::int64_t>(subject.size());
    MatchRange match;

    if (forward) {
        std::int64_t from = hasSelection ? anchorEnd : anchorStart;
        while (from <= subjectLength) {
            if (!regex_->Find(subject, from, match, outcome)) {
                break;
            }
            const bool sameAsSelection = hasSelection && match.start == anchorStart &&
                                         match.end == anchorEnd;
            if (!sameAsSelection) {
                accept(match);
                return true;
            }
            from = (match.end > match.start) ? match.end : match.start + 1;
        }
        if (!outcome.Ok()) {
            return false;
        }
        if (config.wrapAround) {
            std::int64_t wrapFrom = 0;
            while (wrapFrom <= anchorStart && wrapFrom <= subjectLength) {
                if (!regex_->Find(subject, wrapFrom, match, outcome)) {
                    break;
                }
                if (match.start >= anchorStart) {
                    break;
                }
                wrapped = true;
                accept(match);
                return true;
            }
        }
        return false;
    }

    // 正则后向：扫到锚点之前，记住最后一个
    std::int64_t from = 0;
    MatchRange best;
    bool found = false;
    while (from <= subjectLength) {
        MatchRange candidate;
        if (!regex_->Find(subject, from, candidate, outcome)) {
            break;
        }
        if (candidate.end > anchorStart) {
            break;
        }
        best = candidate;
        found = true;
        from = (candidate.end > candidate.start) ? candidate.end : candidate.start + 1;
    }
    if (!outcome.Ok()) {
        return false;
    }
    if (found) {
        accept(best);
        return true;
    }
    if (config.wrapAround) {
        from = anchorStart;
        while (from <= subjectLength) {
            MatchRange candidate;
            if (!regex_->Find(subject, from, candidate, outcome)) {
                break;
            }
            best = candidate;
            found = true;
            from = (candidate.end > candidate.start) ? candidate.end : candidate.start + 1;
        }
        if (found) {
            wrapped = true;
            accept(best);
            return true;
        }
    }
    return false;
}

bool InDocSearch::Count(const SearchConfig& config, int& count, SearchOutcome& outcome) {
    if (!Prepare(config, outcome)) {
        count = 0;
        return false;
    }
    std::vector<MatchRange> matches;
    bool truncated = false;
    if (!CollectMatches(config, matches, kMaxCollectMatches, truncated, outcome)) {
        count = 0;
        return false;
    }
    count = static_cast<int>(matches.size());
    return true;
}

bool InDocSearch::MarkAll(const SearchConfig& config, int& count, SearchOutcome& outcome) {
    ClearMarks();
    if (!Prepare(config, outcome)) {
        count = 0;
        return false;
    }
    std::vector<MatchRange> matches;
    bool truncated = false;
    if (!CollectMatches(config, matches, kMaxCollectMatches, truncated, outcome)) {
        count = 0;
        return false;
    }
    view_.ConfigureSearchIndicators();
    for (const MatchRange& match : matches) {
        if (match.end > match.start) {
            view_.MarkRange(edit::EditView::kIndicatorAllMatches, match.start, match.end);
        }
    }
    count = static_cast<int>(matches.size());
    return true;
}

void InDocSearch::ClearMarks() const {
    view_.ClearIndicator(edit::EditView::kIndicatorAllMatches);
}

bool InDocSearch::ReplaceCurrent(const SearchConfig& config, bool& replaced,
                                 SearchOutcome& outcome) {
    replaced = false;
    if (!Prepare(config, outcome)) {
        return false;
    }

    const std::int64_t selectionStart = view_.SelectionStart();
    const std::int64_t selectionEnd = view_.SelectionEnd();

    // 关键：必须先确认当前选区**确实是一个匹配项**才能替换。
    // 否则用户随手选中一段文字后按"替换"，就会把那一段直接改掉——
    // 这是会破坏用户内容的操作，宁可不动。
    bool selectionIsMatch = false;
    if (selectionEnd > selectionStart) {
        if (config.mode == SearchMode::Regex) {
            const std::string subject = view_.GetAllText();
            MatchRange match;
            const bool found = regex_->Find(subject, selectionStart, match, outcome);
            if (!outcome.Ok()) {
                return false;
            }
            selectionIsMatch = found && match.start == selectionStart && match.end == selectionEnd;
        } else {
            std::int32_t flags = 0;
            if (config.matchCase) {
                flags |= edit::EditView::kSearchFlagMatchCase;
            }
            if (config.wholeWordOnly) {
                flags |= edit::EditView::kSearchFlagWholeWord;
            }
            view_.SetSearchFlags(flags);
            const std::int64_t patternLength = static_cast<std::int64_t>(patternBytes_.size());
            // 把 target 限制在选区内部，命中即说明整段选区就是一个匹配
            view_.SetTarget(selectionStart, selectionEnd);
            const std::int64_t found = view_.SearchInTarget(patternBytes_);
            selectionIsMatch =
                (found == selectionStart && found + patternLength == selectionEnd);
        }
    }

    if (!selectionIsMatch) {
        // 只是"找到下一个"并选中，不做任何修改
        bool wrapped = false;
        FindNext(config, true, wrapped, outcome);
        return outcome.Ok();
    }

    std::string replacement;
    if (config.mode == SearchMode::Regex) {
        const std::string subject = view_.GetAllText();
        if (!regex_->ExpandReplacement(subject, config.replaceTo, replacement, outcome)) {
            return false;
        }
    } else if (!ConvertToDocBytes(config.replaceTo, config.utfMode, config.codePage,
                                  replacement)) {
        outcome = SearchOutcome::Failure(SearchError::EncodingFailed,
                                         L"替换内容无法按当前文档编码表示。");
        return false;
    }

    view_.BeginUndoAction();
    view_.ReplaceSelection(replacement);
    view_.EndUndoAction();
    replaced = true;

    // 替换后顺手选中下一个匹配，方便连续按"替换"
    bool wrapped = false;
    FindNext(config, true, wrapped, outcome);
    return true;
}

bool InDocSearch::ReplaceAll(const SearchConfig& config, int& count, SearchOutcome& outcome) {
    count = 0;
    if (!Prepare(config, outcome)) {
        return false;
    }

    std::vector<MatchRange> matches;
    bool truncated = false;
    if (!CollectMatches(config, matches, kMaxReplacements, truncated, outcome)) {
        return false;
    }
    if (matches.empty()) {
        return true;
    }

    // 先把替换文本全部算出来（正则的 $1 展开依赖"最近一次匹配"，
    // 而接下来要倒序修改缓冲区，匹配数据会失效）。
    std::vector<std::string> replacements;
    replacements.reserve(matches.size());
    if (config.mode == SearchMode::Regex) {
        const std::string subject = view_.GetAllText();
        for (const MatchRange& match : matches) {
            MatchRange located;
            if (!regex_->Find(subject, match.start, located, outcome)) {
                return false;
            }
            std::string text;
            if (!regex_->ExpandReplacement(subject, config.replaceTo, text, outcome)) {
                return false;
            }
            replacements.push_back(std::move(text));
        }
    } else {
        std::string literal;
        if (!ConvertToDocBytes(config.replaceTo, config.utfMode, config.codePage, literal)) {
            outcome = SearchOutcome::Failure(SearchError::EncodingFailed,
                                             L"替换内容无法按当前文档编码表示。");
            return false;
        }
        replacements.assign(matches.size(), literal);
    }

    // 从后往前替换：否则前面的修改会让后面的偏移全部失效
    view_.BeginUndoAction();
    for (std::size_t i = matches.size(); i-- > 0;) {
        view_.SetSelectionRange(matches[i].start, matches[i].end);
        view_.ReplaceSelection(replacements[i]);
    }
    view_.EndUndoAction();

    count = static_cast<int>(matches.size());
    return true;
}

}  // namespace npcc::search
