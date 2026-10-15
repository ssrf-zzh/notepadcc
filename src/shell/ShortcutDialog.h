#pragma once

#include <windows.h>

#include <vector>

#include "core/CommandRegistry.h"

namespace npcc::shell {

struct ShortcutEditResult {
    bool changed = false;   // 用户是否真的改过绑定（决定要不要写 shortcuts.xml）
};

// 管理快捷键（设置 → 管理快捷键）。
//
// registry 会被就地修改：点"确定"保留改动，点"取消"（或 Esc、关窗口）完整回滚。
// defaults 是启动时记录的默认绑定表，供"恢复默认"使用。
// 返回 true 表示用户点了"确定"。
bool ShowShortcutDialog(HWND owner, npcc::core::CommandRegistry& registry,
                        const std::vector<npcc::core::ShortcutBinding>& defaults,
                        ShortcutEditResult* result);

}  // namespace npcc::shell
