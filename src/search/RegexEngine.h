#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "search/SearchTypes.h"

namespace npcc::search {

// PCRE2 的薄封装（设计文档 ADR-04）。
// 用 pimpl 把 pcre2.h 挡在头文件之外——它是 PCRE2 构建时生成的，不该扩散到调用方。
class RegexEngine {
public:
    RegexEngine();
    ~RegexEngine();
    RegexEngine(const RegexEngine&) = delete;
    RegexEngine& operator=(const RegexEngine&) = delete;

    // pattern 为宽字符；按 options.utfMode 决定转成 UTF-8 还是 codePage 字节。
    bool Compile(std::wstring_view pattern, const RegexOptions& options, SearchOutcome& outcome);

    bool Ready() const noexcept;
    bool JitEnabled() const noexcept;

    // 在 subject（编辑器字节）内从 fromOffset 起查找下一个匹配。
    // 返回 false 且 outcome.Ok() 表示"没有匹配"；outcome 有错时才是真失败。
    bool Find(std::string_view subject, std::int64_t fromOffset, MatchRange& match,
              SearchOutcome& outcome);

    // 用最近一次成功的 Find 展开替换串（支持 $1、${name}、$0）。
    bool ExpandReplacement(std::string_view subject, std::wstring_view replacement,
                           std::string& out, SearchOutcome& outcome);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace npcc::search
