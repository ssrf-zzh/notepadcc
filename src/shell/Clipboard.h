#pragma once

#include <windows.h>

#include <string>

namespace npcc::shell {

// 往剪贴板同时写纯文本与富文本（设计文档 2.14 第 4 条）：
// 粘贴到记事本是纯文本、粘贴到 Word 是带色代码。
//
//   plain —— 纯文本（写成 CF_UNICODETEXT）
//   rtf   —— RTF 文本，ASCII、以 NUL 结尾（写成 "Rich Text Format" 注册格式）
//
// 两种格式**一起成功或一起失败**：只写进去一种的话，用户粘到 Word 里拿到的东西
// 取决于 Word 先看哪个格式，那不是我们能猜的。失败时会把剪贴板清空（不让用户拿到
// 一个"看起来像成功"的半成品），并返回 false 让调用方去说明原因。
bool CopyRichTextToClipboard(HWND owner, const std::wstring& plain, const std::string& rtf);

}  // namespace npcc::shell
