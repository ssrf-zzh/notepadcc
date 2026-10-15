#pragma once

#include <windows.h>

namespace npcc::core {
class ConfigCenter;
}

namespace npcc::shell {

// 首选项对话框（设计文档 2.12）。改了配置就写回并落盘（合并写盘）。
// 返回 true 表示用户点了"确定"。
bool ShowPreferencesDialog(HWND owner, npcc::core::ConfigCenter& config);

}  // namespace npcc::shell
