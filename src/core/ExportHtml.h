#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// 文档里的一个字节 + 它的样式号（导出的输入单位）
struct StyledByte {
    char ch = 0;
    int  style = 0;
};

// 一个样式号在导出时的外观
struct ExportStyle {
    int          number = 0;
    std::uint32_t fore = 0;        // 0xRRGGBB
    bool         bold = false;
    bool         italic = false;
};

struct ExportOptions {
    bool          lineNumbers = true;      // 带行号
    std::string   title;                   // <title> 与页眉用的标题（文件名）
    std::uint32_t pageBack = 0xFFFFFF;     // 页面底色
    std::uint32_t pageFore = 0x000000;     // 默认前景色
    std::string   fontFace = "Consolas";
    int           fontSize = 11;
};

// 同一样式连续的一段。两个导出器（HTML / RTF）都要按段输出——一个字符一个 span
// 既臃肿又慢，而且多字节字符被逐字节切开会把 UTF-8 切碎（踩过，见附录 B）
struct StyledRun {
    int         style = 0;
    std::string text;
};

std::vector<StyledRun> GroupStyledBytes(const std::vector<StyledByte>& bytes);

// 把"编辑器缓冲区字节 + 每字节样式号"按 codePage 解成字符、再编成 UTF-8，样式号跟着
// **字符**走。三个导出器（HTML / RTF / 剪贴板）都要先过这一步。
//
// 为什么必须过：编辑器缓冲区里只有 ANSI 文档是原始代码页字节（中文 Windows 上就是 GBK），
// 其余都是 UTF-8。而导出器一律按 UTF-8 解——不转换的话，ANSI 文档里的中文会变成乱码，
// 而且是**不报错的**那种（GBK 的字节按 UTF-8 解出来就是一串看不懂的字符）。
// codePage 传 65001 时原样返回（UTF-8 文档的直通路径）。
std::vector<StyledByte> StyledBytesAsUtf8(const std::vector<StyledByte>& bytes,
                                          std::uint32_t codePage);

// 生成一份**独立的** HTML：样式号 → CSS 类，正文放在 <pre> 里，行号可选。
//
// 纯函数：给"字节 + 样式号 + 样式外观"，吐出字符串。它不认识 Scintilla、也不碰文件，
// 所以导出这条路上最容易出错的部分（转义、样式去重、换行、行号对齐）都能被单测直接钉住。
//
// 用法上的约定：同一次导出里**只给用到的样式号**建类（`s7` 这种），没用到的样式不写进 CSS；
// 行号用与正文相同的行高排右对齐，复制出去时不会带行号噪音（<pre> 里的行号是单独的表格列）。
std::string BuildHtml(const std::vector<StyledByte>& bytes,
                      const std::vector<ExportStyle>& styles, const ExportOptions& options);

// HTML 里要转义的三个字符（`<` `>` `&`）与引号。暴露出来是为了让单测能单独钉转义规则
std::string EscapeHtmlText(std::string_view text);

}  // namespace npcc::core
