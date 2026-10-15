#include "core/CommandRegistry.h"

#include <algorithm>

#include "edit/DocBuffer.h"
#include "edit/EditView.h"
#include "infra/Log.h"

namespace npcc::core {

CmdId CommandRegistry::Register(Command command) {
    if (command.id == 0) {
        // 自动分配必须绕开已经用掉的号：显式 ID 是"类别 + 序号"编出来的，
        // 高类别（比如工具菜单）占的号很可能正好落在自动分配的下一号上。
        // 撞号的后果是**后来者被静默丢弃**——表现就是"菜单里有这一项，点了没反应"。
        while (Find(nextAutoId_) != nullptr) {
            ++nextAutoId_;
        }
        command.id = nextAutoId_++;
    } else {
        nextAutoId_ = std::max(nextAutoId_, command.id + 1);
    }
    if (Find(command.id) != nullptr) {
        // 重复注册必须出声：静默保留先来者会变成"菜单里有这一项，点了没反应"
        infra::LogWarn(L"cmd", L"命令 ID 重复，已忽略后来者: " + std::to_wstring(command.id) +
                                   L" / " + command.key);
        return command.id;   // 重复注册：保留先注册者，返回既有 id
    }
    commands_.push_back(std::move(command));
    return commands_.back().id;
}

const Command* CommandRegistry::Find(CmdId id) const {
    const auto it = std::find_if(commands_.begin(), commands_.end(),
                                 [id](const Command& c) { return c.id == id; });
    return it == commands_.end() ? nullptr : &*it;
}

std::vector<CmdId> CommandRegistry::ByCategory(CmdCategory category) const {
    std::vector<CmdId> result;
    for (const auto& c : commands_) {
        if (c.category == category) {
            result.push_back(c.id);
        }
    }
    return result;
}

ExecResult CommandRegistry::Execute(CmdId id, const CmdContext& context) {
    Command* command = const_cast<Command*>(Find(id));
    if (command == nullptr) {
        return ExecResult::NotFound;
    }
    if (!IsEnabled(id, context)) {
        return ExecResult::Disabled;
    }
    if (!command->execute) {
        return ExecResult::Unsupported;
    }
    command->execute(context);
    return ExecResult::Ok;
}

bool CommandRegistry::IsEnabled(CmdId id, const CmdContext& context) const {
    const Command* command = Find(id);
    if (command == nullptr) {
        return false;
    }
    if (command->queryEnabled) {
        return command->queryEnabled(context);
    }
    // 无自定义判定时，按 flags 做通用前置条件检查
    if ((command->flags & kCmdNeedsBuffer) != 0 && context.buffer == nullptr) {
        return false;
    }
    if ((command->flags & kCmdNeedsWritable) != 0 &&
        (context.buffer == nullptr || context.buffer->isReadOnly)) {
        return false;
    }
    if ((command->flags & kCmdNeedsSelection) != 0) {
        if (context.view == nullptr || context.view->SelectionStart() == context.view->SelectionEnd()) {
            return false;
        }
    }
    return true;
}

bool CommandRegistry::IsChecked(CmdId id, const CmdContext& context) const {
    const Command* command = Find(id);
    if (command == nullptr || (command->flags & kCmdCheckable) == 0) {
        return false;
    }
    return command->queryChecked ? command->queryChecked(context) : false;
}

bool CommandRegistry::BindShortcut(CmdId id, uint8_t modifiers, uint16_t keyCode, bool replace) {
    if (Find(id) == nullptr || keyCode == 0) {
        return false;
    }
    auto same = [modifiers, keyCode](const ShortcutBinding& b) {
        return b.modifiers == modifiers && b.keyCode == keyCode;
    };
    if (replace) {
        bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(), same), bindings_.end());
    } else if (std::any_of(bindings_.begin(), bindings_.end(), same)) {
        return false;
    }
    bindings_.push_back(ShortcutBinding{id, modifiers, keyCode});
    return true;
}

std::vector<CmdId> CommandRegistry::FindConflicts(uint8_t modifiers, uint16_t keyCode) const {
    std::vector<CmdId> result;
    for (const auto& b : bindings_) {
        if (b.modifiers == modifiers && b.keyCode == keyCode) {
            result.push_back(b.cmdId);
        }
    }
    return result;
}

CmdId CommandRegistry::ResolveShortcut(uint8_t modifiers, uint16_t keyCode) const {
    for (const auto& b : bindings_) {
        if (b.modifiers == modifiers && b.keyCode == keyCode) {
            return b.cmdId;
        }
    }
    return 0;
}

std::vector<ShortcutBinding> CommandRegistry::BindingsOf(CmdId id) const {
    std::vector<ShortcutBinding> result;
    for (const auto& b : bindings_) {
        if (b.cmdId == id) {
            result.push_back(b);
        }
    }
    return result;
}

void CommandRegistry::ClearShortcuts() {
    bindings_.clear();
}

void CommandRegistry::ClearShortcutsOf(CmdId id) {
    bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                   [id](const ShortcutBinding& b) { return b.cmdId == id; }),
                    bindings_.end());
}

bool CommandRegistry::UnbindShortcut(CmdId id, uint8_t modifiers, uint16_t keyCode) {
    const auto before = bindings_.size();
    bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                   [id, modifiers, keyCode](const ShortcutBinding& b) {
                                       return b.cmdId == id && b.modifiers == modifiers &&
                                              b.keyCode == keyCode;
                                   }),
                    bindings_.end());
    return bindings_.size() != before;
}

void CommandRegistry::AssignShortcut(CmdId id, uint8_t modifiers, uint16_t keyCode) {
    ClearShortcutsOf(id);
    BindShortcut(id, modifiers, keyCode, true);
}

void CommandRegistry::SetShortcuts(std::vector<ShortcutBinding> bindings) {
    bindings_ = std::move(bindings);
}

}  // namespace npcc::core
