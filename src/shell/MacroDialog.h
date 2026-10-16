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

// 保存运行命令时问名字（设计文档 2.13 第 3 条）。
// 放在这里是因为"一行字 + 一个输入框 + 两个按钮"那套小对话框的实现就在这个文件里，
// 再抄一份没有意义。返回 false = 用户取消。
bool AskRunCommandName(HWND owner, const std::wstring& suggested, std::wstring& out);

// 十六进制视图里"跳到偏移"（设计文档 2.13 第 5 条）。同理，复用那套小对话框。
// 这里只负责把用户敲的字原样交回去，**解析留给调用方**（edit::ParseHexJump）。
bool AskHexOffset(HWND owner, std::wstring& out);

}  // namespace npcc::shell
