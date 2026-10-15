#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace npcc::search {

// 文档缓冲区存的是**文件原始字节**（UTF-8 或某个 ANSI 代码页）。
// 查找模式串与替换串来自界面（宽字符），必须按文档编码转换后再匹配，
// 否则在 GBK 文档里搜中文永远搜不到。

bool ConvertToDocBytes(std::wstring_view text, bool utfMode, std::uint32_t codePage,
                       std::string& out);

std::wstring ConvertFromDocBytes(std::string_view bytes, bool utfMode, std::uint32_t codePage);

}  // namespace npcc::search
