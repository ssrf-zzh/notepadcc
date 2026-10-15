#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace npcc::edit {

class EditView;

// 矩形选区上的批量操作。
//
// 共同约束：**必须从最后一行往第一行做**。先改前面的行会让后面行的位置
// 整体偏移，之后每一步都落错地方——这是这一块最典型的坑。

// 在矩形选区的每一行插入对应文本（文本按行顺序给出），返回实际处理的行数。
int ApplyColumnInsert(EditView& view, const std::vector<std::wstring>& texts, bool utfMode,
                      std::uint32_t codePage);

// 删除矩形选区覆盖的每一行内容，返回实际处理的行数。
int DeleteColumnSelection(EditView& view);

}  // namespace npcc::edit
