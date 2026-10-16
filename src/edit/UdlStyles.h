#pragma once

// UDL（用户自定义语言）用的样式号（设计文档 2.6 第 3 条 / 7.6）。
//
// 这一组号是**我们自己定的**，不是 Lexilla 的 SCE_*：UDL 的词法器是自研的
// （锁定的 Lexilla 版本里没有 container 词法器，见设计说明书 7.6 末尾的改判），
// 所以样式号的含义由我们说了算——这也正是"样式配置器按样式号逐项调整"将来能
// 成立的前提：自己定的号才有稳定、可解释的含义。
//
// 号段之间留了空档（每组隔开 10），将来加"第 10 组关键字""文档注释"这类样式
// 不必把已有号搬家——样式号一旦写进配置文件就不好再动了。

namespace npcc::edit::udl {

// 关键字组数与 Scintilla 的 KEYWORDSET_MAX 对齐（0..8）
inline constexpr int kKeywordGroupCount = 9;

inline constexpr int kStyleDefault      = 0;    // 普通文本
inline constexpr int kStyleCommentLine  = 1;    // 行注释
inline constexpr int kStyleCommentBlock = 2;    // 块注释
inline constexpr int kStyleKeywordBase  = 10;   // 10..18：关键字组 0..8
inline constexpr int kStyleNumber       = 20;   // 数字
inline constexpr int kStyleString       = 21;   // 字符串（所有界定符共用一个样式）
inline constexpr int kStyleOperator     = 22;   // 运算符
inline constexpr int kStyleIdentifier   = 23;   // 普通标识符

inline constexpr int KeywordStyleOf(int group) { return kStyleKeywordBase + group; }

// 界面、日志与文档里用的名字（"注释（行）"这类）。未定义的样式号返回 "?"。
const char* StyleName(int style);

}  // namespace npcc::edit::udl
