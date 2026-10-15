#pragma once

#include <cstdint>
#include <string>

namespace npcc::search {

// 三种搜索模式（设计文档 2.5 第 1 条）
enum class SearchMode : std::uint8_t {
    Literal,   // 纯字面量，走 Scintilla 原生搜索
    Escaped,   // 先解析 \n \t \xNN 这类转义，再走字面量路径
    Regex,     // PCRE2
};

enum class SearchError {
    None,
    EmptyPattern,
    BadPattern,
    MatchLimitExceeded,
    EncodingFailed,
    InternalError,
};

struct SearchOutcome {
    SearchError  error = SearchError::None;
    std::wstring message;

    bool Ok() const noexcept { return error == SearchError::None; }
    static SearchOutcome Success() { return {}; }
    static SearchOutcome Failure(SearchError code, std::wstring text) {
        SearchOutcome outcome;
        outcome.error = code;
        outcome.message = std::move(text);
        return outcome;
    }
};

// 匹配区间，单位是编辑器缓冲区的**字节**偏移。
// 注意：编辑器缓冲区存的是文件原始字节（UTF-8 或 ANSI），
// 所以偏移不是字符数——这正是 Scintilla 的坐标约定。
struct MatchRange {
    std::int64_t start = 0;
    std::int64_t end = 0;
};

// 正则引擎的编译与匹配选项
struct RegexOptions {
    bool          matchCase = false;
    bool          dotMatchesNewline = false;
    bool          wholeWordOnly = false;   // 实现方式是把模式包成 \b(?:...)\b
    bool          utfMode = false;      // 缓冲区是 UTF-8 时置位
    std::uint32_t codePage = 936;       // 非 UTF 模式下用于转换模式串与替换串
};

}  // namespace npcc::search
