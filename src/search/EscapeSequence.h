#pragma once

#include <string>
#include <string_view>

#include "search/SearchTypes.h"

namespace npcc::search {

// 把"扩展模式"下的转义序列解析为实际字节：
//   \n \r \t \0 \\ \xNN \x{NNNN}
// 不认识的转义原样保留反斜杠（与用户直觉一致，避免静默吞字符）。
// pattern 是 UTF-8 字节；返回的也是字节。
bool UnescapeExtended(std::string_view pattern, std::string& out, SearchOutcome& outcome);

}  // namespace npcc::search
