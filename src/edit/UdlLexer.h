#pragma once

// Lexilla 的 ILexer5 依赖 Sci_Position 这些类型，它自己不包含 ILexer.h
#include <ILexer.h>

#include "edit/UdlDefinition.h"

namespace npcc::edit {

// 造一个 UDL 词法器（设计文档 2.6 第 3 条 / 7.6）。
//
// 返回的指针**交给 Scintilla 托管**：装上之后由它负责 Release，调用方不要 delete，
// 也不要在装上之后再碰它。词法器的存活期由 Scintilla 决定（换语言、销毁控件时释放）。
Scintilla::ILexer5* CreateUdlLexer(const UdlDefinition& definition);

}  // namespace npcc::edit
