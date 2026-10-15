#pragma once

#include <vector>

#include "core/Command.h"

namespace npcc::core {

// 全程序唯一的命令注册表：菜单、快捷键、工具栏、宏、插件最终都落到这里。
class CommandRegistry {
public:
    // id 为 0 时自动分配。重复 id 会被拒绝（返回已存在的 id）。
    CmdId Register(Command command);

    const Command* Find(CmdId id) const;
    const std::vector<Command>& All() const noexcept { return commands_; }
    std::vector<CmdId> ByCategory(CmdCategory category) const;

    ExecResult Execute(CmdId id, const CmdContext& context);
    bool IsEnabled(CmdId id, const CmdContext& context) const;
    bool IsChecked(CmdId id, const CmdContext& context) const;

    bool BindShortcut(CmdId id, uint8_t modifiers, uint16_t keyCode, bool replace);
    const std::vector<ShortcutBinding>& AllBindings() const noexcept { return bindings_; }
    std::vector<CmdId> FindConflicts(uint8_t modifiers, uint16_t keyCode) const;
    CmdId ResolveShortcut(uint8_t modifiers, uint16_t keyCode) const;

private:
    std::vector<Command>         commands_;
    std::vector<ShortcutBinding> bindings_;
    CmdId                        nextAutoId_ = 1;
};

}  // namespace npcc::core
