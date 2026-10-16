#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/CommandRegistry.h"
#include "core/ToolbarItems.h"

namespace npcc::shell {

// 工具栏定制（设计文档 2.11 第 6 条："工具栏按钮集合与顺序由配置文件驱动，
// 提供可视化定制界面"）。
//
// 左边是"还能放上去的命令"，右边是"现在放着的按钮"；顺序就是列表里的顺序。
// 候选来自命令注册表——所以宏、运行命令、插件命令一样能摆到工具栏上。
// 同一套界面也服务**右键菜单定制**（M3 第九块）：候选表与整理规则完全共用，
// 标题、两行说明与"恢复默认"的默认表按目标换一份。
struct ToolbarPicks {
    core::ToolbarItems items;
    bool custom = true;   // false = 回到代码里的默认按钮表（而不是"空表"）
};

// 返回 true = 用户点了确定（out 被填上）；false = 取消（out 不动）
bool CustomizeToolbar(HWND owner, const core::CommandRegistry& registry,
                      const core::ToolbarItems& current, ToolbarPicks& out);

// 右键菜单定制：与工具栏共用同一套对话框与整理规则，
// 只是标题 / 列表说明 / "恢复默认"的默认表换成右键菜单那一份。
bool CustomizeContextMenu(HWND owner, const core::CommandRegistry& registry,
                          const core::ToolbarItems& current, ToolbarPicks& out);

}  // namespace npcc::shell
