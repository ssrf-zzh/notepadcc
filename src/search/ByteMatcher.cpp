#include "search/ByteMatcher.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "search/EscapeSequence.h"
#include "search/TextBytes.h"

namespace npcc::search {
namespace {

// 全字匹配的字符类：ASCII 字母数字下划线，以及所有非 ASCII 字节
// （把多字节字符整体当作词字符，这样中文/日文也能得到符合直觉的边界判断）。
bool IsWordByte(unsigned char ch) {
    return std::isalnum(static_cast<int>(ch)) != 0 || ch == '_' || ch >= 0x80;
}

// 字面量搜索的大小写折叠只覆盖 ASCII。
// 真正需要 Unicode 折叠时请用正则模式——PCRE2 在 UTF+UCP 下的 CASELESS 是完整的。
void FoldAsciiInPlace(std::string& text) {
    for (char& ch : text) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (value < 0x80) {
            ch = static_cast<char>(std::tolower(value));
        }
    }
}

bool EqualAt(std::string_view bytes, std::size_t offset, std::string_view pattern,
             bool caseInsensitive) {
    if (offset + pattern.size() > bytes.size()) {
        return false;
    }
    if (!caseInsensitive) {
        return std::memcmp(bytes.data() + offset, pattern.data(), pattern.size()) == 0;
    }
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(bytes[offset + i]);
        const unsigned char b = static_cast<unsigned char>(pattern[i]);
        if (a < 0x80) {
            a = static_cast<unsigned char>(std::tolower(a));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

}  // namespace

ByteMatcher::ByteMatcher() = default;
ByteMatcher::~ByteMatcher() = default;

bool ByteMatcher::Matches(const SearchConfig& config, bool utfMode, std::uint32_t codePage) const {
    return ready_ && config_.pattern == config.pattern && config_.mode == config.mode &&
           config_.matchCase == config.matchCase &&
           config_.wholeWordOnly == config.wholeWordOnly &&
           config_.dotMatchesNewline == config.dotMatchesNewline && utfMode_ == utfMode &&
           codePage_ == codePage;
}

bool ByteMatcher::Prepare(const SearchConfig& config, bool utfMode, std::uint32_t codePage,
                          SearchOutcome& outcome) {
    outcome = SearchOutcome::Success();
    config_ = config;
    utfMode_ = utfMode;
    codePage_ = codePage;
    ready_ = false;
    pattern_.clear();
    patternRaw_.clear();
    regex_.reset();

    if (config.pattern.empty()) {
        outcome = SearchOutcome::Failure(SearchError::EmptyPattern, L"搜索内容为空。");
        return false;
    }

    std::string raw;
    if (!ConvertToDocBytes(config.pattern, utfMode, codePage, raw)) {
        outcome = SearchOutcome::Failure(SearchError::EncodingFailed,
                                         L"搜索内容无法按目标编码表示。");
        return false;
    }

    switch (config.mode) {
        case SearchMode::Literal:
            patternRaw_ = raw;
            break;
        case SearchMode::Escaped:
            if (!UnescapeExtended(raw, patternRaw_, outcome)) {
                return false;
            }
            break;
        case SearchMode::Regex: {
            RegexOptions options;
            options.utfMode = utfMode;
            options.codePage = codePage;
            options.matchCase = config.matchCase;
            options.dotMatchesNewline = config.dotMatchesNewline;
            options.wholeWordOnly = config.wholeWordOnly;
            regex_ = std::make_unique<RegexEngine>();
            if (!regex_->Compile(config.pattern, options, outcome)) {
                regex_.reset();
                return false;
            }
            ready_ = true;
            return true;
        }
    }

    if (patternRaw_.empty()) {
        outcome = SearchOutcome::Failure(SearchError::EmptyPattern, L"搜索内容为空。");
        return false;
    }
    pattern_ = patternRaw_;
    if (!config.matchCase) {
        FoldAsciiInPlace(pattern_);
    }
    ready_ = true;
    return true;
}

bool ByteMatcher::FindLiteralAll(std::string_view bytes, std::vector<MatchRange>& matches,
                                 std::size_t limit, bool& truncated) {
    const bool caseInsensitive = !config_.matchCase;
    const std::size_t patternLength = pattern_.size();
    const std::size_t length = bytes.size();

    for (std::size_t i = 0; i + patternLength <= length;) {
        if (matches.size() >= limit) {
            truncated = true;
            break;
        }
        if (!EqualAt(bytes, i, pattern_, caseInsensitive)) {
            ++i;
            continue;
        }
        if (config_.wholeWordOnly) {
            const bool leftOk = (i == 0) ||
                                !IsWordByte(static_cast<unsigned char>(bytes[i - 1]));
            const std::size_t after = i + patternLength;
            const bool rightOk = (after >= length) ||
                                 !IsWordByte(static_cast<unsigned char>(bytes[after]));
            if (!leftOk || !rightOk) {
                ++i;
                continue;
            }
        }
        matches.push_back(MatchRange{static_cast<std::int64_t>(i),
                                     static_cast<std::int64_t>(i + patternLength)});
        i += patternLength;
    }
    return true;
}

bool ByteMatcher::FindAll(std::string_view bytes, std::vector<MatchRange>& matches,
                          std::size_t limit, bool& truncated, SearchOutcome& outcome) {
    matches.clear();
    truncated = false;
    outcome = SearchOutcome::Success();

    if (!ready_) {
        outcome = SearchOutcome::Failure(SearchError::InternalError, L"匹配器尚未准备。");
        return false;
    }
    if (config_.mode != SearchMode::Regex) {
        return FindLiteralAll(bytes, matches, limit, truncated);
    }

    std::int64_t from = 0;
    const std::int64_t length = static_cast<std::int64_t>(bytes.size());
    while (from <= length) {
        if (matches.size() >= limit) {
            truncated = true;
            break;
        }
        MatchRange match;
        if (!regex_->Find(bytes, from, match, outcome)) {
            return outcome.Ok();
        }
        matches.push_back(match);
        from = (match.end > match.start) ? match.end : match.start + 1;
    }
    return true;
}

bool ByteMatcher::ExpandReplacement(std::string_view bytes, std::int64_t matchStart,
                                    std::wstring_view replacement, std::string& out,
                                    SearchOutcome& outcome) {
    out.clear();
    outcome = SearchOutcome::Success();

    if (!ready_) {
        outcome = SearchOutcome::Failure(SearchError::InternalError, L"匹配器尚未准备。");
        return false;
    }

    if (config_.mode == SearchMode::Regex) {
        if (regex_ == nullptr) {
            outcome = SearchOutcome::Failure(SearchError::InternalError, L"正则尚未编译。");
            return false;
        }
        MatchRange located;
        if (!regex_->Find(bytes, matchStart, located, outcome)) {
            return outcome.Ok();
        }
        return regex_->ExpandReplacement(bytes, replacement, out, outcome);
    }

    // 字面量模式没有捕获组，替换串本身就是结果
    if (!ConvertToDocBytes(replacement, utfMode_, codePage_, out)) {
        outcome = SearchOutcome::Failure(SearchError::EncodingFailed,
                                         L"替换内容无法按该文件的编码表示。");
        return false;
    }
    return true;
}

}  // namespace npcc::search
