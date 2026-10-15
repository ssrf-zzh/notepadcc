#pragma once

#include <windows.h>

#include "core/CommandRegistry.h"

namespace npcc::shell {

// 由命令注册表生成主菜单：菜单项文本、ID、快捷键提示三者同源，
// 因此重绑快捷键后菜单提示会同步变化（设计文档 1.3 的三条硬规则之一）。
HMENU BuildMainMenu(const core::CommandRegistry& registry);

}  // namespace npcc::shell
