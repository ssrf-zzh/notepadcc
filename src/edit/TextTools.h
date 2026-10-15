#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::edit {

// 行操作与文本转换（设计文档 2.13 的 M12）。
//
// 全部是**纯函数**：给字节、返回字节，不碰窗口也不做 IO，所以能被单独测。
// 按"编辑器缓冲区里的字节"处理是刻意的——缓冲区对 UTF-8 / UTF-16 文档来说就是 UTF-8，
// 对 ANSI 文档来说是代码页字节；需要按字符理解的地方（大小写转换）显式传代码页进来。
//
// 两条贯穿所有行操作的约定：
//   1. **保留原文的行尾风格**：CRLF 的文本排完序还是 CRLF，不会被悄悄换成 LF；
//   2. **保留结尾换行与否**：原文末尾有换行就保留、没有就不加——这类细节用户一眼就能看出来。

struct LineSplit {
    std::vector<std::string> lines;    // 不含行尾
    std::string              eol;      // 探测到的行尾："\r\n" / "\n" / "\r"
    bool                     trailingEol = false;   // 原文是否以行尾结束
};

LineSplit SplitLines(std::string_view text);
std::string JoinLines(const std::vector<std::string>& lines, const std::string& eol,
                      bool trailingEol);

enum class SortOrder : uint8_t { Ascending, Descending };

// ---- 行操作 ----
std::string SortLines(std::string_view text, SortOrder order, bool caseSensitive);
std::string SortLinesByLength(std::string_view text, SortOrder order);
// 只删**真正空**的行（长度为 0）。只有空白的行不算空行，与对标产品一致。
std::string RemoveEmptyLines(std::string_view text);
// keepLast = true 时保留最后一次出现（位置也用最后一次的位置）
std::string RemoveDuplicateLines(std::string_view text, bool keepLast);
std::string ReverseLines(std::string_view text);
std::string TrimTrailingWhitespace(std::string_view text);
std::string TrimLeadingWhitespace(std::string_view text);
// 行号加前缀，形如 "  1\t正文"。padWidth > 0 时按宽度右对齐补空格
std::string AddLineNumbers(std::string_view text, long long start, long long step, int padWidth);
// 删行号：只认"行首一串数字 + 一个分隔符（制表符 / 空格 / 点 / 冒号）"这种形态，
// 免得把正文开头的数字也一起砍掉
std::string StripLineNumbers(std::string_view text);

// ---- 注释（需要语言的行注释/块注释符，见 LanguageRegistry）----
// 行注释开关：所有非空行都已经带着前缀就取消，否则加上。前缀插在缩进之后；
// 取消时连同前缀后面的一个空格一起摘掉（这样"加注释→取消注释"是往返的）。
std::string ToggleLineComment(std::string_view text, std::string_view prefix);
// 块注释开关：整段已经被 start/end 包住就脱掉，否则包上
std::string ToggleBlockComment(std::string_view text, std::string_view start,
                               std::string_view end);

// ---- 大小写（按代码页解释字节，非 ASCII 字符交给系统的大小写映射）----
std::string ToUpperCase(std::string_view text, std::uint32_t codePage);
std::string ToLowerCase(std::string_view text, std::uint32_t codePage);

// ---- 转换工具 ----
std::string Base64Encode(std::string_view bytes);
// 失败（含非法字符或长度不合法）返回 false，不改动 out
bool Base64Decode(std::string_view text, std::string& out);
// 只对"未保留字符"之外做百分号编码；解码只认 %XX（'+' 保持原样，不做表单式转换）
std::string UrlEncode(std::string_view text);
bool UrlDecode(std::string_view text, std::string& out);
std::string EscapeHtml(std::string_view text);
std::string UnescapeHtml(std::string_view text);

}  // namespace npcc::edit
