#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/Command.h"

namespace npcc::core {

// 命令的显示文本里带着菜单助记符与结尾的快捷键提示，例如
//   L"保存(&S)"            -> L"保存"
//   L"列块编辑(&C)..."      -> L"列块编辑"
//   L"关于 notepadcc(&A)"  -> L"关于 notepadcc"
// 快捷键管理界面要显示命令名，菜单要显示"名称(&X)提示"，两边都从这里取，
// 免得各写一套解析逻辑后慢慢长歪。
std::wstring StripAccelerator(std::wstring_view text);

// 类别名（不带助记符）。菜单与快捷键管理界面共用同一张表。
// 名字走语言包（键是 cat.*）；包缺这一条时用代码里的简体中文默认文本。
std::wstring CategoryText(CmdCategory category);

// 类别的语言包键（cat.file 这类）。给"缺键自检"用。
const wchar_t* CategoryKey(CmdCategory category);

// 菜单里类别的助记符字母。
wchar_t CategoryMnemonic(CmdCategory category);

// 快捷键的显示文本："Ctrl+Alt+Shift+S"。修饰键顺序固定，keyCode 为 0 时返回空串。
// 键名交给 GetKeyNameText，因此和系统键盘布局保持一致。
std::wstring ShortcutText(uint8_t modifiers, uint16_t keyCode);

}  // namespace npcc::core
