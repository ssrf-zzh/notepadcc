#pragma once

#include <windows.h>

namespace npcc::plugin {
class PluginHost;
}

namespace npcc::shell {

// 插件管理器（设计文档 2.10 第 5 条）：列出发现到的插件与状态，允许禁用/启用。
//
// 返回 true = 用户改了启用状态并已写盘。**改动下次启动生效**——插件已经把回调留在了
// 宿主里，卸库等于给悬空指针开门（设计里也是这么定的：重启才能完整重载）。
bool ShowPluginManager(HWND owner, const npcc::plugin::PluginHost& plugins);

}  // namespace npcc::shell
