#pragma once

#include <memory>

#include "edit/DocBuffer.h"
#include "edit/EditView.h"

namespace npcc::edit {

// 一份文档 = 状态（DocBuffer） + 一个 Scintilla 控件。
// 每个缓冲区独占一个控件，这样撤销历史、滚动位置、折叠状态都随文档保留，
// 切换标签只是显示/隐藏，不需要重建（设计文档 3.2）。
struct Document {
    DocBuffer                   buffer;
    std::unique_ptr<EditView>   view;
};

}  // namespace npcc::edit
