#pragma once

#include <functional>
#include <string>
#include <vector>

namespace npcc::core {

// 工具栏按钮表（设计文档 2.11 第 6 条：按钮集合与顺序由配置文件驱动）。
//
// 表里存的是**命令键**而不是命令 ID：ID 是外壳跑起来之后才分配出来的（宏、插件命令、
// 运行命令都是如此），存 ID 的话下次启动就对不上号；键是稳定字符串，
// `macro.我的宏`、`run.编译`、`plugin.xxx` 这类也能一样地记——这正是设计里
// "工具栏与右键菜单定制"要顺带解决的那三个遗留（宏 / 运行命令 / 插件都没绑工具栏）。
//
// 一个元素要么是一条命令键，要么是分隔符 "-"。
using ToolbarItems = std::vector<std::wstring>;

inline constexpr const wchar_t* kToolbarSeparator = L"-";

// 代码里写的那份默认按钮表（配置文件里没有给出 <Item> 时用它）
const ToolbarItems& DefaultToolbarItems();

// 右键菜单的默认项表（M3 第九块"右键菜单深度定制"）。它就是同一套东西：
// 存命令键、"-" 是分隔符、NormalizeToolbarItems 就是它的整理器——候选里
// 同样包含宏 / 运行命令 / 插件命令，所以"哪些能进右键菜单"与"哪些能上工具栏"
// 是同一个答案。
const ToolbarItems& DefaultContextMenuItems();

// 把一份"用户给的"按钮表整理成能用的：键不存在的丢掉、重复的只留第一次、
// 分隔符不站在开头 / 结尾、两个分隔符不挨着。known 回答"这个键现在在不在"。
//
// 返回空表是**合法**结果——那表示"工具栏上一个按钮都不放"，与"整理失败"不是一回事。
ToolbarItems NormalizeToolbarItems(const ToolbarItems& requested,
                                   const std::function<bool(const std::wstring&)>& known);

}  // namespace npcc::core
