#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "search/SearchTypes.h"   // SearchMode

namespace npcc::search {
struct SearchConfig {
    std::wstring pattern;
    std::wstring replaceTo;
    SearchMode   mode = SearchMode::Literal;
    bool         matchCase = false;
    bool         wholeWordOnly = false;
    bool         dotMatchesNewline = false;
    bool         wrapAround = true;
    bool         inSelectionOnly = false;

    // 由调用方按文档编码设置：UTF-8 文档置 utfMode，ANSI 文档给 codePage。
    // 跨文件搜索时这两个字段由每个文件自己的编码决定，此处不生效。
    bool          utfMode = true;
    std::uint32_t codePage = 936;

    // ---- 跨文件搜索（设计文档 3.4） ----
    std::vector<std::wstring> directories;
    // 分号分隔，支持 !前缀表示排除，例如 "*.js;*.html;!node_modules"
    std::wstring filter = L"*.*";
    bool         includeSubDirs = true;
    bool         skipBinary = true;
    std::uint64_t maxFileSizeBytes = 64ull * 1024 * 1024;
    std::size_t   maxFiles = 200000;
    std::size_t   maxHitsPerFile = 5000;
};

}  // namespace npcc::search
