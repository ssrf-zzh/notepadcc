#pragma once

#include <string_view>

#include "core/JsonFormat.h"   // FormatResult

namespace npcc::core {

// XML 美化（设计文档 2.13 第 2 条）。
//
// 三条底线：
//  1. **只动空白，不动内容**：标签、属性、注释、CDATA、处理指令、DOCTYPE 全部原样搬运。
//  2. **混合内容不重排**：`<p>hello <b>world</b></p>` 这种"元素里夹着文字"的地方
//     **整段原样输出**——往里塞缩进等于往用户的文本里塞空白，那不叫格式化，叫改数据。
//     判据只看**直接子节点**：`<a><b>文字</b></a>` 里的 a 仍会重排（文字在 b 里面），
//     否则现实里几乎每份 XML 都会因为"叶子元素带文字"而整体拒排。
//     带 CDATA 的元素算混合内容（CDATA 里装的就是字符数据），整段不动。
//  3. 失败时不返回半截文本，给出第几行第几列的说明。
FormatResult FormatXml(std::string_view text, int indent = 2);

}  // namespace npcc::core
