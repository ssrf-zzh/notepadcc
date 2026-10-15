#pragma once

#include <string>
#include <string_view>

namespace npcc::io {

struct WriteResult {
    bool          ok = false;
    unsigned long lastError = 0;
    bool          diskReadOnly = false;
    bool          accessDenied = false;
    bool          diskFull = false;
    std::wstring  message;
};

// 原子写盘（设计文档 4.6 的七步契约）：失败时绝不改变原文件内容。
WriteResult WriteFileAtomic(const std::wstring& path, std::string_view bytes);

std::wstring DescribeWin32Error(unsigned long code);

}  // namespace npcc::io
