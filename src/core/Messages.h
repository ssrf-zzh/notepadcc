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
    kMsgReplacePlan     = WM_APP + 11,  // 替换计划（指针所有权转移给 UI 线程）
    kMsgReplaceFinished = WM_APP + 12,
    kMsgStartupRecovery = WM_APP + 13,  // 窗口显示之后再问"要不要恢复上次未保存的内容"
    kMsgCrashTest       = WM_APP + 14,  // 自检开关：等窗口就绪后故意崩一次
    kMsgUserRangeStart  = WM_APP + 100, // 以下保留给外壳内部
};

}  // namespace npcc::core
