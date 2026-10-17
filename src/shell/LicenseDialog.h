#pragma once

#include <windows.h>

#include <string>

#include "infra/ThreadPool.h"

namespace npcc::shell {

// 许可证对话框（B6，《notepadcc许可与激活设计.md》8.4）：自绘模态框，
// 窗口类名 notepadccLicenseDialog（界面回归脚本用 FindWindowW 找它）。
// 四个分区：
//   1) 状态     —— 版本 / 持有人 / 席位 / 有效期 / 机器短码 / 最近的错误说明；
//   2) 在线激活 —— 授权码 + 服务地址，后台线程请求（超时 10 秒、只重试一次）；
//   3) 离线激活 —— 席位与单位名 → 申请串（复制 / 另存为 txt）；
//   4) 导入     —— 选一个 .lic / .dat 文件，走 LicenseService::Import。
//
// 在线激活结果用 core::kMsgLicenseActivateResult 投回**本对话框**（指针所有权
// 一并转移，与更新 / 远程对话框同一条约定），模态循环照常分发消息。
//
// serviceUrl 是 UTF-8；空 = 配置里没写——点"激活"会明确提示改用离线激活，
// 一个字节都不会发出去（设计 6.1：任何网络失败都不影响程序可用）。
//
// 对话框期间许可文件被换过（在线激活成功或导入成功）时 Outcome.changed 为
// true：调用方拿到它后要 RebuildMenu()，让菜单跟着新状态走。
struct LicenseDialogOutcome {
    bool changed = false;
};

class LicenseDialog {
public:
    static LicenseDialogOutcome Show(HWND owner, infra::ThreadPool& pool, std::string serviceUrl);
};

}  // namespace npcc::shell
