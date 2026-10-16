#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <atomic>

#include "infra/ThreadPool.h"
#include "infra/UpdateService.h"

namespace npcc::shell {

// 检查更新对话框（M3 第十块）：自绘模态框，窗口类名 notepadccUpdateCheck。
// 两条入口：
//  1) 菜单「检查更新…」：ShowChecking 立刻把对话框开出来，检查丢给线程池，
//     结果用 core::kMsgUpdateResult 投回**对话框自己**（模态循环照常分发消息）；
//  2) 启动延迟检查找到新版本：ShowAvailable 把现成的报告直接摆出来。
// 状态行是普通 STATIC：回归脚本用 WM_GETTEXT 逐字读回来核对结果文案。
class UpdateDialog {
public:
    // busy 是"已有一次检查在进行"的共享标志：提交前置位、结果回来清位。
    // 调用方（MainWindow）自己那份自动检查也用同一个标志。
    static void ShowChecking(HWND owner, infra::ThreadPool& pool,
                             const std::shared_ptr<std::atomic<bool>>& busy,
                             std::string sourceUrl, std::string currentVersion);

    // 直接展示一份已有结果（启动自动检查那条路；目前只有"发现新版本"会走到这里）。
    static void ShowAvailable(HWND owner, const infra::UpdateCheckReport& report);
};

}  // namespace npcc::shell
