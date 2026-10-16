#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/RunCommand.h"

namespace npcc::shell {

// F5 运行命令（设计文档 2.13 第 3 条）。
//
// 对话框只负责"问用户要一条命令行"，不做展开也不启动进程——那两件事在外壳里，
// 这样这个框可以被界面回归脚本直接驱动（填字、点按钮），而不必去猜展开结果。
enum class RunDialogAction {
    Run,    // 现在就跑
    Save,   // 存成命名命令（不跑）
};

struct RunDialogResult {
    RunDialogAction action = RunDialogAction::Run;
    std::wstring    command;            // 命令行原文（含变量，未展开）
    std::wstring    name;               // action == Save 时的命令名
    bool            captureOutput = true;
};

// savedCommands 里已有的命令会列在下拉里（选中即把它的命令行填进来，
// 连同它自己的"捕获输出"设置）。initial 是上次用过的命令行。
// 返回 false = 用户取消。
bool ShowRunCommandDialog(HWND owner, const std::vector<core::RunCommandDef>& savedCommands,
                          const std::wstring& initial, RunDialogResult& out);

// 保存运行命令时问名字。
bool AskRunCommandName(HWND owner, const std::wstring& suggested, std::wstring& out);

}  // namespace npcc::shell
