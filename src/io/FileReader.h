#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace npcc::io {

struct ReadResult {
    bool          ok = false;
    uint64_t      size = 0;
    std::string   bytes;
    unsigned long lastError = 0;
    std::wstring  message;
    bool          tooLarge = false;
};

// 整文件读入内存。M0 直接全量读取；大文件的分档策略（ILoader / 映射 / 只读视图）
// 属于设计文档 6.1 的 T2/T3，安排在 M1。
ReadResult ReadWholeFile(const std::wstring& path, uint64_t maxBytes = 64ull * 1024 * 1024);

}  // namespace npcc::io
