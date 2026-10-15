#pragma once

#include <cstdint>

namespace npcc::core {

// 全部为不透明标识：任何代码不得假设它们等于容器下标。
using BufferId = int32_t;
using ViewId   = int32_t;
using CmdId    = uint32_t;
using MacroId  = int32_t;

inline constexpr int32_t kInvalidId = -1;

// 命令 ID 编码：高 4 位为类别，低 12 位为类别内序号。
// 必须落在 16 位以内——菜单项 ID（WM_COMMAND 的 LOWORD）与 ACCEL.cmd 都是 WORD。
// 插件命令若超出 16 位，由宿主另行分配一个菜单槽位并做映射（见设计文档附录 B）。
inline constexpr CmdId MakeCmdId(uint8_t category, uint16_t index) {
    return (static_cast<CmdId>(category) << 12) | (index & 0x0FFFu);
}

}  // namespace npcc::core
