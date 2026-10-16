#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace npcc::core {

// 打印分页（设计文档 2.14 第 1 条）。
//
// 排版本身交给 Scintilla 的 SCI_FORMATRANGE：问它"从第 N 个字节开始，这一页能排到哪"，
// 它连换行、行高、制表位一起算好——比自己按"行高 × 可用高度"估算准得多（差异记在设计文档里）。
// 这里负责的是**外层那圈循环**：收每页的起点、防死循环。
//
// pageEnd：给一个起始字节，回答"这一页排到了哪"。返回 -1 表示排不动了。
// 返回值必须**前进**，否则停下——不前进还继续循环的话，表现是打印任务卡死、纸一直出，
// 所以这条守卫是硬的（单测里专门钉住）。
std::vector<int32_t> CollectPageStarts(int32_t length,
                                       const std::function<intptr_t(int32_t)>& pageEnd);

}  // namespace npcc::core
