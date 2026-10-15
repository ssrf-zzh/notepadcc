#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "search/SearchTypes.h"

namespace npcc::search {

// 命中行的信息：行号（1 基）、行文本、行内匹配区间（相对行首的字节偏移）
struct LineHit {
    std::int32_t              line = 0;
    std::string               text;
    std::vector<MatchRange>   matches;
};

struct FileSearchResult {
    std::wstring            path;
    std::uint64_t           fileSize = 0;
    std::vector<LineHit>    hits;
    bool                    truncated = false;   // 单文件命中过多被截断
    std::wstring            error;               // 非空表示这个文件出错了
    bool                    binary = false;      // 按二进制跳过
};

struct SearchProgress {
    std::uint64_t filesTotal = 0;
    std::uint64_t filesScanned = 0;
    std::uint64_t filesMatched = 0;
    std::uint64_t hits = 0;
    std::uint64_t skippedBinary = 0;
    std::uint64_t failedFiles = 0;
    std::wstring  currentFile;
    bool          finished = false;
    bool          cancelled = false;
};

}  // namespace npcc::search
