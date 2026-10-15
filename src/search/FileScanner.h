#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "io/FileReader.h"
#include "search/SearchConfig.h"

namespace npcc::search {

// 目录遍历（在文件中查找 / 替换共用）。cancelled 返回 true 时提前结束。
void EnumerateFiles(const SearchConfig& config, std::vector<std::wstring>& out,
                    const std::function<bool()>& cancelled);

// 一个已经读进来、并按自身编码准备好内容的文件
struct PreparedFile {
    std::string   bytes;          // 编辑器域字节：UTF-8，或 ANSI 原字节
    bool          utfMode = true; // true 表示 UTF-8 系列，false 表示按 codePage 解释
    std::uint32_t codePage = 936;
    bool          binary = false; // 按二进制跳过
    std::wstring  error;          // 非空表示读取失败
    std::uint64_t fileSize = 0;
    std::uint64_t lastWriteMs = 0;
};

bool PrepareFile(const std::wstring& path, const SearchConfig& config, PreparedFile& out);

// 文件身份查询复用 io 层实现，避免两处各写一份
using npcc::io::QueryFileIdentity;

// 行映射工具
std::vector<std::size_t> BuildLineStarts(std::string_view text);
std::size_t              LineIndexFor(const std::vector<std::size_t>& lineStarts,
                                      std::size_t offset);
std::string_view         LineTextAt(std::string_view text,
                                    const std::vector<std::size_t>& lineStarts,
                                    std::size_t lineIndex);

}  // namespace npcc::search
