#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/CommandRegistry.h"

namespace npcc::core {

// shortcuts.xml：快捷键的持久化（设计文档 7.3）。
//
// 语义是"**相对默认方案的覆盖**"，不是"完整的快捷键表"：
//   * 文件里出现的命令 → 以文件为准（先清空该命令的默认绑定再套用）
//   * 文件里没出现的命令 → 保持注册表里已有的默认绑定
//   * Key="0" → 该命令明确不绑定快捷键
// 最后一条是关键：没有它，"删掉一个快捷键"存盘后再读回来会凭空复活。
// 之后新增的命令也不会因为没有记录而失去默认快捷键。
//
// 顶层元素沿用对标产品的 <NotepadPlusCompat> 与 Ctrl/Alt/Shift/Key 属性拼写，
// 但命令是用我们自己的 name（命令键）与 id 标识的，所以**还不能直接吞下
// 对标产品的 shortcuts.xml**（它的 id 是另一套编号）。这件事记在
// 设计文档附录 D，对应基线的"快捷键迁移预设"。
class ShortcutStore {
public:
    static std::filesystem::path Path();

    // 文件不存在不算失败（首次运行就该用默认方案）。
    bool Load(CommandRegistry& registry, std::wstring* error = nullptr,
              std::vector<std::wstring>* warnings = nullptr) const;

    bool Save(const CommandRegistry& registry, std::wstring* error = nullptr) const;
};

// 允许绑定到命令的按键组合：必须带 Ctrl 或 Alt，或者本身是功能键 / Insert / Delete。
//
// 这条规则不是洁癖，是为了挡住两类真实的坏结果：
//   1. 单按一个字母就把命令绑走 —— 之后在编辑器里打字会不停触发命令；
//   2. 把 Home/End/PgUp/PgDn 这些键占掉 —— 快捷键列表和编辑器都没法导航了。
// Shift 单独配字母同样不放行（Shift+A 就是大写 A），Shift+F3 这类没问题。
bool IsBindableShortcut(uint8_t modifiers, uint16_t keyCode);

}  // namespace npcc::core
