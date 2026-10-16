#pragma once

#include <string>
#include <vector>

#include "edit/StyleScheme.h"

namespace npcc::edit {

struct LanguageDef;

// 一个语言里"可以逐项调整的样式号"。
//
// 这一层解决的是"用户想调某个具体的样式号，却不知道它是什么"：词法器只认数字，
// 目录给出「数字 → 名字 → 默认属于哪个配色角色」。
//
// 两条边界要说清楚：
// ① **目录不是权威**：stylers.xml 里写下的任何样式号都会被应用（哪怕目录里没有它）。
//    目录只负责"界面上列得出来、点了能改"，文件才是最终话语权——这样用户手写一个
//    冷门样式号也能生效，不必等我们把他要的那一项补进目录。
// ② 内置语言列出的是**本程序真的上过色的那些样式**（每族 5~7 项），不是 Lexilla
//    的全部样式号（一个词法器动辄几十个号，全列出来只是噪音）。
struct StyleEntry {
    int          number = 0;   // Scintilla 样式号
    std::wstring name;         // 界面显示（"行注释"）
    StyleRole    role = StyleRole::Comment;   // 没被单独改过时用的配色角色
};

// 该语言可逐项调整的样式清单（按样式号升序）。认不出的语言返回空表。
// 每次调用现算（最多十几项）——语言切换、开配置器才用到，不值得为它维护缓存。
std::vector<StyleEntry> StylesFor(const LanguageDef& language);

}  // namespace npcc::edit
