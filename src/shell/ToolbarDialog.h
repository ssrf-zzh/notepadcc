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
struct ToolbarPicks {
    core::ToolbarItems items;
    bool custom = true;   // false = 回到代码里的默认按钮表（而不是"空表"）
};

// 返回 true = 用户点了确定（out 被填上）；false = 取消（out 不动）
bool CustomizeToolbar(HWND owner, const core::CommandRegistry& registry,
                      const core::ToolbarItems& current, ToolbarPicks& out);

}  // namespace npcc::shell
