#include "core/CommandRegistry.h"

#include <algorithm>

#include "edit/DocBuffer.h"
#include "edit/EditView.h"

namespace npcc::core {

CmdId CommandRegistry::Register(Command command) {
    if (command.id == 0) {
        command.id = nextAutoId_++;
    } else {
        nextAutoId_ = std::max(nextAutoId_, command.id + 1);
    }
    if (Find(command.id) != nullptr) {
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

}  // namespace npcc::core
