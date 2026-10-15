#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace npcc::infra {

// 单实例（设计文档 2.3 第 6 条 / 附录 C 的 kMsgOpenFiles）。
//
// 做法：一个具名互斥体决定谁是第一个实例；后到的实例把命令行里的文件路径用
// WM_COPYDATA 交给已在跑的那个，然后自己退出。选 WM_COPYDATA 是因为它是
// **系统会替我们封送指针**的消息——WM_APP 系列不会（见附录 B 的跨进程陷阱）。
//
// 交接窗口比主窗口建得早：宿主窗口在 wWinMain 里最先创建，文件先攒着，
// 等主窗口建好（SetMainWindow）再转过去。否则"双击文件的瞬间程序还在启动"
// 这种最常见的时序就会丢文件。

// 返回 true = 本进程是第一个实例；false = 已有实例在跑，参数已经交给它了。
// className 是接收方的窗口类名（主窗口类名），multiInstance 为 true 时不参与抢占。
bool ClaimSingleInstance(const std::wstring& className, const std::vector<std::wstring>& files,
                         bool multiInstance);

// 主窗口建好后调用：后续交接直接投递给它，并立刻处理排队中的文件。
void SetSingleInstanceTarget(HWND mainWindow);

// 取走"排队等着要打开"的文件路径（取走后清空）
std::vector<std::wstring> TakePendingFiles();

// 打包/解析交接用的字节流（两进程之间只有它需要保证格式一致，单独测它）
std::string PackPaths(const std::vector<std::wstring>& paths);
std::vector<std::wstring> UnpackPaths(std::string_view bytes);

}  // namespace npcc::infra
