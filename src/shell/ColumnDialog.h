#pragma once

#include <windows.h>

#include <vector>

#include "edit/ColumnText.h"

namespace npcc::shell {

enum ColumnDialogControl : int {
    kColumnDialogBase = 750,
    kColumnTemplateEdit = 751,
    kColumnStartEdit = 752,
    kColumnStepEdit = 753,
    kColumnPadEdit = 754,
    kColumnBaseCombo = 755,
    kColumnResetEdit = 756,
    kColumnPreviewList = 757,
    kColumnInsertButton = 758,
    kColumnDeleteButton = 759,
    kColumnCancelButton = 760,
    kColumnDialogLimit = 779,
};

struct ColumnDialogResult {
    bool                       apply = false;          // 用户点了"插入"
    bool                       deleteColumn = false;   // 用户点了"删除列块内容"
    npcc::edit::ColumnInsertOptions options;
};

// 列块编辑对话框（Alt+C）。lineCount 是当前矩形选区覆盖的行数，用于预览。
// 返回 false 表示用户取消。
bool ShowColumnDialog(HWND owner, int lineCount, ColumnDialogResult& result);

}  // namespace npcc::shell
