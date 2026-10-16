#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace npcc::edit {

// 只读十六进制视图的排版与解析（设计文档 2.13 第 5 条）。
//
// 全是纯函数：给字节算出一行该怎么排、把"跳到这个偏移"的输入解析成数字、把鼠标点
// 到的列换成第几个字节。这些东西在界面上没法调，但错一个空格整列就错位、错一位
// 偏移就跳到别处，所以必须单独测。
//
// 一行的样子（列是 0 基，全部按**字符**数，不是像素）：
//
//   00000010  4A 6F 20 62 69 6E 00 00  00 00 00 00 00 00 00 00  Jo bin..........
//   ^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^ ^^^^
//    偏移 8    十六进制 16 字节（第 8 个字节后多一个空档）    ASCII
//
// 偏移与字节都用大写十六进制（与常见十六进制编辑器一致）；ASCII 一列里可打印字符
// 原样显示（0x20~0x7E），其余一律一个 '.'——**不能**原样打出去：控制字符会把界面
// 弄得一团糟，而 0x80 以上的字节按单字节编码打出来是乱码。

inline constexpr int kHexBytesPerRow = 16;
inline constexpr int kHexHalf = 8;              // 第 8 个字节之后的空档
inline constexpr int kHexOffsetStart = 0;
inline constexpr int kHexOffsetWidth = 8;        // 偏移的字符宽度（超过 4 GB 的文件用 16）
inline constexpr int kHexTextStart = kHexOffsetWidth + 2;
inline constexpr int kHexTextWidth = kHexBytesPerRow * 3;   // 含分隔空格，末尾那个也算
inline constexpr int kHexAsciiStart = kHexTextStart + kHexTextWidth + 2;
inline constexpr int kHexRowWidth = kHexAsciiStart + kHexBytesPerRow;

// 偏移要几位：超过 4 GB 的文件 8 位不够用
int HexOffsetWidthFor(std::uint64_t fileSize);
// 大写的定宽十六进制偏移
std::string FormatHexOffset(std::uint64_t offset, int width = kHexOffsetWidth);

// 一行文本（不含行尾）。length 是本行的实际字节数（1~16）；不足 16 的部分用空格补齐，
// 这样最后一行的 ASCII 列仍然对得齐。length 为 0 时返回全空档的一行。
std::string HexRowText(const std::uint8_t* data, std::size_t length, std::uint64_t offset,
                       int offsetWidth = kHexOffsetWidth);

// 若干行拼成一块（行间用 \r\n）。data 是**整块缓冲区的起点**、length 是它的长度，
// startOffset 是要从这里开始的偏移——**别再传一个已经偏移过的指针进来**：
// 那样偏移会被应用两次，屏幕上显示的就是另一段字节（踩过，见 HexView::VisibleText）。
// startOffset + byteCount 超出实际长度时按实际长度截断。
std::string HexBlockText(const std::uint8_t* data, std::size_t length, std::uint64_t startOffset,
                         std::uint64_t byteCount, int offsetWidth = kHexOffsetWidth);

// 把"跳到某处"的输入解析成偏移。
//   "0x1F" / "0X1f" → 十六进制（**必须带 0x**：不带前缀的按十进制，免得"10"到底是
//   十六还是十要靠猜）
//   忽略首尾空白；空串、非法字符、溢出都返回 false（且不动 out）
bool ParseHexJump(std::wstring_view text, std::uint64_t& out);

// 鼠标点在某一行的第 column 个**字符**上，对应本行第几个字节。
// rowBytes 是本行的实际字节数；点在本行没有字节的位置（补齐的空档上）返回 false。
bool HexByteAtColumn(int column, int rowBytes, int* indexInRow);

}  // namespace npcc::edit
