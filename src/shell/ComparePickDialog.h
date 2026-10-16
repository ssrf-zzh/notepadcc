#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace npcc::shell {

// 选一个"要跟当前文档比"的已打开文档（设计文档第 10 章 M3 的 Compare）。
//
// 只列**已经打开的文档**，不去弹系统文件对话框：系统那个"打开"对话框是 DirectUI 的，
// 跨进程脚本驱动不了（见设计文档 9.3 记的测试能力边界），而我们自己的列表既能测、
// 对用户也更贴近实际用法（"我刚打开的两个文件比一比"）。
//
// names 是候选文档的显示名；currentName 用于在提示里说明"跟谁比"。
// 返回 false = 用户取消；否则 out 是 names 里的下标。
bool PickDocumentToCompare(HWND owner, const std::vector<std::wstring>& names,
                           const std::wstring& currentName, std::size_t& out);

}  // namespace npcc::shell
