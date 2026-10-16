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

bool CommandRegistry::UpdateCommand(CmdId id, std::wstring text,
                                    std::function<void(const CmdContext&)> execute) {
    for (Command& command : commands_) {
        if (command.id == id) {
            command.text = std::move(text);
            if (execute) {
                command.execute = std::move(execute);
            }
            return true;
        }
    }
    return false;
}

bool CommandRegistry::SetCommandText(CmdId id, std::wstring text) {
    for (Command& command : commands_) {
        if (command.id == id) {
            command.text = std::move(text);
            return true;
        }
    }
    return false;
}

bool CommandRegistry::Unregister(CmdId id) {
    const auto firstRemoved = std::remove_if(
        commands_.begin(), commands_.end(), [id](const Command& c) { return c.id == id; });
    if (firstRemoved == commands_.end()) {
        return false;
    }
    commands_.erase(firstRemoved, commands_.end());
    // 命令没了，绑在它身上的快捷键也必须一起走：留着的绑定是"幽灵命令"，
    // 加速键按下会走进一个不存在的命令
    ClearShortcutsOf(id);
    return true;
}

const Command* CommandRegistry::Find(CmdId id) const {
    const auto it = std::find_if(commands_.begin(), commands_.end(),
                                 [id](const Command& c) { return c.id == id; });
    return it == commands_.end() ? nullptr : &*it;
}

const Command* CommandRegistry::FindByKey(std::wstring_view key) const {
    if (key.empty()) {
        return nullptr;
    }
    const auto it = std::find_if(commands_.begin(), commands_.end(),
                                 [key](const Command& c) { return c.key == key; });
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
    // 录制钩子放在"执行之后"：没执行成功的命令（被禁用、不存在）不该进宏，
    // 否则回放时会莫名其妙地多出一步
    if (recordHook_ && (command->flags & kCmdRecordable) != 0) {
        recordHook_(*command);
    }
    return ExecResult::Ok;
}

bool CommandRegistry::IsEnabled(CmdId id, const CmdContext& context) const {
    const Command* command = Find(id);
    if (command == nullptr) {
        return false;
    }
    // 上下文门禁放在最前面：带 queryEnabled 的命令也拦得住——不然"T3 只读视图里
    // 不许碰编辑器"这条规则会被任何一条自定义判定悄悄绕过去
    if (contextGate_ && !contextGate_(id, context)) {
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
