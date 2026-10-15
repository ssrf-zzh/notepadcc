#pragma once

#include <windows.h>

namespace npcc::core {

// 自定义消息集中定义（设计文档附录 C）。禁止在别处直接书写 WM_APP + n，
// 否则很容易发生取值冲突。
enum : UINT {
    kMsgOpenFiles       = WM_APP + 1,   // 单实例转交文件路径（M1 后续）
    kMsgSearchProgress  = WM_APP + 2,
    kMsgSearchResult    = WM_APP + 3,
    kMsgTaskFinished    = WM_APP + 4,
    kMsgFileChanged     = WM_APP + 5,
    kMsgPluginNotify    = WM_APP + 6,
    kMsgAppActivate     = WM_APP + 7,
    kMsgSearchFinished  = WM_APP + 8,
    kMsgSearchWakeup    = WM_APP + 9,   // 进度更新：UI 线程收到后自己去读进度
    kMsgUserRangeStart  = WM_APP + 100, // 以下保留给外壳内部
};

}  // namespace npcc::core
