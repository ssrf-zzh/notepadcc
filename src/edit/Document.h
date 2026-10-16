#pragma once

#include <memory>

#include "edit/DocBuffer.h"
#include "edit/EditView.h"
#include "edit/TextLogView.h"

namespace npcc::edit {

// 一份文档 = 状态（DocBuffer） + 一个 Scintilla 控件。
// 每个缓冲区独占一个控件，这样撤销历史、滚动位置、折叠状态都随文档保留，
// 切换标签只是显示/隐藏，不需要重建（设计文档 3.2）。
//
// T3 超大文件档（设计 6.1）**不用 Scintilla**：内容不复制进文档模型，
// 改用自绘只读视口（TextLogView，底下是内存映射 + 延迟行索引）。
// 此时 view 仍然存在（空文档），只为让"所有文档都有 EditView"这条不变量成立；
// 显示与交互走 logView，见 Document::LogViewActive()。
struct Document {
    DocBuffer                   buffer;
    std::unique_ptr<EditView>   view;
    std::unique_ptr<TextLogView> logView;   // 仅 T3 档创建（见 DocumentManager::LoadInto）

    bool LogViewActive() const noexcept { return logView != nullptr; }
};

}  // namespace npcc::edit
