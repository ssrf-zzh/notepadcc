#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/CommandRegistry.h"
#include "core/Macro.h"
#include "core/RunCommand.h"

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
//
// 同一个文件里还有 <Macros> 一节（设计文档 7.3）。**这份文件只有一个写者**：
// 宏由外壳通过 SetMacros() 交给它，保存时一起写出去。这样"改快捷键"那一路
// （ShortcutDialog → Save）不会把宏顺手抹掉——把两份内容分给两个写者，
// 迟早会出现"存了快捷键，宏没了"。
//
// <UserDefinedCommands>（运行命令，设计文档 2.13 第 3 条）同理：由外壳通过
// SetRunCommands() 交进来，保存时一起写。
//
// 运行命令的快捷键**只写在 <Command> 的 Ctrl/Alt/Shift/Key 属性里**，不写进
// <InternalCommands>：读文件时 <InternalCommands> 是"按命令键查注册表"的，
// 而运行命令要到外壳注册之后才存在，写在那儿会在下次启动时认不出来、绑定被丢掉。
// 这一点与宏一致（宏的快捷键也在 <Macro> 上，不在 <InternalCommands> 里）。
class ShortcutStore {
public:
    static std::filesystem::path Path();

    // 文件不存在不算失败（首次运行就该用默认方案）。读进来的宏留在对象里，随 Save 写回。
    bool Load(CommandRegistry& registry, std::wstring* error = nullptr,
              std::vector<std::wstring>* warnings = nullptr);

    bool Save(const CommandRegistry& registry, std::wstring* error = nullptr) const;

    // 当前这份宏表。Load 之后是文件里的那些；随后外壳可以增删（保存宏时）。
    const std::vector<Macro>& Macros() const noexcept { return macros_; }
    void SetMacros(std::vector<Macro> macros) { macros_ = std::move(macros); }
    // 追加一条并返回它的下标；同名时覆盖（宏名就是它的标识）
    std::size_t AddOrReplaceMacro(Macro macro);

    // 当前这份运行命令表。外壳注册命令时把它们变成菜单项/快捷键。
    const std::vector<RunCommandDef>& RunCommands() const noexcept { return runCommands_; }
    void SetRunCommands(std::vector<RunCommandDef> commands) {
        runCommands_ = std::move(commands);
    }
    // 追加一条并返回它的下标；同名时覆盖（命令名就是它的标识）
    std::size_t AddOrReplaceRunCommand(RunCommandDef command);
    // 保存之前由外壳调一次：把注册表里**实际的**快捷键回填进命令记录，
    // 这样用户在"管理快捷键"里绑的键才落得进文件（与写宏时的做法一致）
    void SyncRunCommandShortcuts(const CommandRegistry& registry);

private:
    std::vector<Macro>         macros_;
    std::vector<RunCommandDef> runCommands_;
};

// 允许绑定到命令的按键组合：必须带 Ctrl 或 Alt，或者本身是功能键 / Insert / Delete。
//
// 这条规则不是洁癖，是为了挡住两类真实的坏结果：
//   1. 单按一个字母就把命令绑走 —— 之后在编辑器里打字会不停触发命令；
//   2. 把 Home/End/PgUp/PgDn 这些键占掉 —— 快捷键列表和编辑器都没法导航了。
// Shift 单独配字母同样不放行（Shift+A 就是大写 A），Shift+F3 这类没问题。
bool IsBindableShortcut(uint8_t modifiers, uint16_t keyCode);

}  // namespace npcc::core
