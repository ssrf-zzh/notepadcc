#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::core {

// 函数列表里的一个符号（设计文档 2.6 第 5 条）。
//
// **这不是编译器**：提取器是"轻量结构提取"，即"大多数语言靠一点点语法常识 + 大括号/缩进
// 深度就够了"。所以它只保证"真实代码里的函数与类能列出来、层级大致正确"，
// 不保证处理宏、模板特化、多行声明这些边角——设计文档里也是这么定的。
struct FunctionSymbol {
    std::wstring name;      // 显示名（"main"、"MyClass"、"## 用法"）
    int          line = 0;  // 0 起的行号（跳转直接用它）
    int          depth = 0; // 嵌套层级（0 = 顶层）
    std::wstring kind;      // "函数" / "类" / "标题" / "选择器"
};

// 按语言名提取符号。语言不认识时返回空表（**不报错**——"这种语言列不出来"是正常状态）。
//
// text 是文档的原始字节（UTF-8），函数名按字节切出来后转成宽字符；
// 这样提取器完全不需要知道文档的编码与行尾风格。
std::vector<FunctionSymbol> ExtractFunctionList(std::wstring_view language,
                                                std::string_view text);

// 这份语言能不能列出符号（界面用它决定要不要显示面板、要不要弹"这种语言列不出来"）
bool HasFunctionExtractor(std::wstring_view language);

}  // namespace npcc::core
