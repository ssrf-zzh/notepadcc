#pragma once

#include <windows.h>

#include <string>

namespace npcc::shell {

// XPath 查询对话框（M3 内置 XML 工具里 XPath 那一件，设计文档 2.13）。
//
// 对话框只负责"问表达式、列结果、给出选了哪一行"：求值在 core::XPathQuery
// （纯函数），跳转由调用方执行——这样界面回归脚本可以逐条核对列表文字与
// 状态栏说明，而不必去猜渲染。
//
// 结果行格式固定为"第 N 行  预览"（两个空格），预览来自 core::FormatXPathNodePreview；
// 双击一行或选中后按"跳转"即关闭对话框并返回那一行。每跑一次查询就写一条
// 日志（成功记命中数、失败记错误原文），用户连跑几条也能在日志里逐条对上。
struct XPathDialogResult {
    bool accepted = false;  // 选了"跳转"（或双击）
    int  line = 1;          // 要跳到的行（1 基）
};

// utf8Text 是当前文档全文（调用方在打开前取一次；对话框是模态的，期间文档不会再变）。
// 返回 false = 用户没选跳转（直接关掉了）。
bool ShowXPathDialog(HWND owner, const std::string& utf8Text, XPathDialogResult& out);

}  // namespace npcc::shell
