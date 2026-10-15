#pragma once

#include <windows.h>

namespace npcc::edit {
class LanguageRegistry;
class StyleScheme;
}  // namespace npcc::edit

namespace npcc::shell {

// 样式配置器（设计文档 6.4）。按**语义角色**调整配色：注释 / 关键字 / 数字 / 字符串 /
// 强调项 / 次要项，每项可以改前景色与粗体斜体。
//
// 为什么按角色而不是按 Lexilla 的样式号：样式号是词法器内部的事（同一个 lexer 给不同
// 语言用时样式号也不同），用户想调的是"注释用什么颜色"。
//
// 返回 true = 用户点了确定，此时 scheme 已被就地修改（调用方负责落盘与重新上色）。
bool ShowStyleConfigurator(HWND owner, const npcc::edit::LanguageRegistry& languages,
                           npcc::edit::StyleScheme& scheme);

}  // namespace npcc::shell
