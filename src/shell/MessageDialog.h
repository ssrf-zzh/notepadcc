#pragma once

#include <windows.h>

namespace npcc::shell {

// 与 MessageBoxW 参数 / 返回值兼容的自绘消息框。返回 IDOK / IDCANCEL /
// IDYES / IDNO；图标与按钮组合只解释外壳实际用到的几档。
int ShowMessageBox(HWND owner, const wchar_t* text, const wchar_t* title, UINT type);

// 回归脚本用它精确定位消息框；窗口类不随系统 MessageBox 的实现变化。
inline constexpr const wchar_t* kMessageDialogClass = L"notepadccMessageDialog";

}  // namespace npcc::shell
