#pragma once

#include <string>
#include <vector>

#include "core/ExportHtml.h"   // StyledByte / ExportStyle / ExportOptions

namespace npcc::core {

// 生成 RTF（设计文档 2.14 第 3 条）：样式号 → RTF 颜色表 + 字体表项，
// 正文按"同一样式连续段"输出，保留前景色与粗斜体。
//
// 纯函数，与 BuildHtml 吃同一份输入——两边的配色口径因此不可能不一致。
// 行号不进 RTF（RTF 的行号要另起一列文本，粘到 Word 里反而碍事），
// 这一点与 HTML 导出的差别如实写在设计文档里。
std::string BuildRtf(const std::vector<StyledByte>& bytes,
                     const std::vector<ExportStyle>& styles, const ExportOptions& options);

}  // namespace npcc::core
