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

// 把文档字节转成 UTF-8。搜索结果里的行文本统一用 UTF-8 传递，
// 界面层就不必再关心每个文件原本是什么编码。
std::string DocBytesToUtf8(std::string_view bytes, bool utfMode, std::uint32_t codePage);

// 判断文本能否在给定代码页下无损表示。
// 实现方式是一次往返转换比对——WideCharToMultiByte 遇到无法表示的字符会
// 静默替换成 '?' 而不报错，只靠返回值判断不出来。
bool CanRepresentInCodePage(std::wstring_view text, std::uint32_t codePage);

}  // namespace npcc::search
