#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "search/RegexEngine.h"
#include "search/SearchConfig.h"
#include "search/SearchTypes.h"

namespace npcc::search {

// 在**字节缓冲区**上做匹配（不依赖 Scintilla，因而不要求 UI 线程）。
// 跨文件搜索用它在工作线程里扫描文件内容。
//
// 与 InDocSearch 的分工：InDocSearch 面向编辑器控件，字面量走 Scintilla 原生搜索；
// 本类面向内存中的字节，字面量自己做朴素搜索。
class ByteMatcher {
public:
    ByteMatcher();
    ~ByteMatcher();
    ByteMatcher(const ByteMatcher&) = delete;
    ByteMatcher& operator=(const ByteMatcher&) = delete;

    bool Prepare(const SearchConfig& config, bool utfMode, std::uint32_t codePage,
                 SearchOutcome& outcome);

    // 是否与上一次 Prepare 的参数一致（供线程内缓存复用）
    bool Matches(const SearchConfig& config, bool utfMode, std::uint32_t codePage) const;

    bool Ready() const noexcept { return ready_; }

    // 找出 bytes 内全部匹配（受 limit 限制）。空匹配会强制前进一字节。
    bool FindAll(std::string_view bytes, std::vector<MatchRange>& matches, std::size_t limit,
                 bool& truncated, SearchOutcome& outcome);

private:
    bool FindLiteralAll(std::string_view bytes, std::vector<MatchRange>& matches,
                        std::size_t limit, bool& truncated);

    SearchConfig                 config_;
    bool                         utfMode_ = true;
    std::uint32_t                codePage_ = 936;
    bool                         ready_ = false;
    std::string                  pattern_;      // 已按需做大小写折叠
    std::string                  patternRaw_;   // 用于全字匹配的边界判断
    std::unique_ptr<RegexEngine> regex_;
};

}  // namespace npcc::search
