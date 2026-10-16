#pragma once

#include <windows.h>

#include <string>

namespace npcc::shell {

// 停止录制之后给宏取个名字（设计文档 5.7 录制流程第 6 步）。
// 返回 false = 用户取消（此时应当丢弃这条录制）。
bool AskMacroName(HWND owner, const std::wstring& suggested, std::wstring& out);

struct MacroRunOptions {
    int  repeatCount = 1;            // 重复执行多少遍（1~10000）
    bool stopOnError = true;         // 失败即停；false = 跳过失败步骤继续（宽松模式）
};

// 回放之前问一次"重复几遍、失败怎么办"（对应基线的"重复执行 N 次"）。
// 返回 false = 用户取消。
bool AskMacroRunOptions(HWND owner, const std::wstring& macroName, MacroRunOptions& out);

}  // namespace npcc::shell
