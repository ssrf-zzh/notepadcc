#pragma once

#include <vector>

#include "core/Command.h"

namespace npcc::core {

// 全程序唯一的命令注册表：菜单、快捷键、工具栏、宏、插件最终都落到这里。
class CommandRegistry {
public:
    // id 为 0 时自动分配。重复 id 会被拒绝（返回已存在的 id）。
    CmdId Register(Command command);

    // 就地改一条命令的显示文本与执行体（宏列表会随录制变化：重名覆盖后菜单项要跟着变）。
    // 只放开这两样：类别、flags、快捷键都不该被外部随手改。返回 false = 没这条命令。
    bool UpdateCommand(CmdId id, std::wstring text, std::function<void(const CmdContext&)> execute);

    // 只换显示文本（切换界面语言时用）。返回 false = 没这条命令。
    bool SetCommandText(CmdId id, std::wstring text);

    const Command* Find(CmdId id) const;
    const std::vector<Command>& All() const noexcept { return commands_; }
    std::vector<CmdId> ByCategory(CmdCategory category) const;

    ExecResult Execute(CmdId id, const CmdContext& context);
    bool IsEnabled(CmdId id, const CmdContext& context) const;
    bool IsChecked(CmdId id, const CmdContext& context) const;

    // 录制钩子（设计文档 2.9）：宏录制期间由外壳装上，凡是带 kCmdRecordable 的命令
    // 在执行成功之后回调一次。钩子放在这里而不是各个命令的实现里，是因为 Execute()
    // 是全程序唯一的执行入口——菜单、快捷键、宏自己都从这里过。
    using RecordHook = std::function<void(const Command&)>;
    void SetRecordHook(RecordHook hook) { recordHook_ = std::move(hook); }

    bool BindShortcut(CmdId id, uint8_t modifiers, uint16_t keyCode, bool replace);
    const std::vector<ShortcutBinding>& AllBindings() const noexcept { return bindings_; }
    std::vector<CmdId> FindConflicts(uint8_t modifiers, uint16_t keyCode) const;
    CmdId ResolveShortcut(uint8_t modifiers, uint16_t keyCode) const;

    // ---- 快捷键管理界面要用的编辑操作 ----
    // 一个命令可以有多条绑定（如"下一个标签"同时有 Ctrl+Tab 与 Ctrl+PgDn），
    // 所以"改绑"必须能精确到"清掉这个命令的全部绑定"或"只摘掉某一个组合"。
    std::vector<ShortcutBinding> BindingsOf(CmdId id) const;
    void ClearShortcuts();
    void ClearShortcutsOf(CmdId id);
    bool UnbindShortcut(CmdId id, uint8_t modifiers, uint16_t keyCode);
    void AssignShortcut(CmdId id, uint8_t modifiers, uint16_t keyCode);   // 替换该命令的全部绑定
    void SetShortcuts(std::vector<ShortcutBinding> bindings);             // 整表替换（用于取消时回滚）

private:
    std::vector<Command>         commands_;
    std::vector<ShortcutBinding> bindings_;
    CmdId                        nextAutoId_ = 1;
    RecordHook                   recordHook_;
};

}  // namespace npcc::core
