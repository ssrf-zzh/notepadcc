#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/RemoteSite.h"
#include "infra/ThreadPool.h"

namespace npcc::shell {

// 远程站点浏览对话框（M3 第十一块）：自绘模态框，窗口类名 notepadccRemoteBrowse。
// 一个框里做完"浏览 → 下载打开 / 上传 / 建目录 / 改名 / 删除"这几件事：
//   - 站点下拉框列出配置里的站点（config.xml 的 <remote> 节），切换即重连；
//   - 目录列表是"当前目录"（远程路径永远 POSIX 风格，见 core/RemoteSite）；
//   - "下载并打开"把文件拉进本地缓存（<配置根>\remote\<站点>\…），关框后由
//     主窗口走常规的打开流程——之后它就是一份普通本地文档，"上传当前文档"
//     再按缓存路径反推回远程路径传回去；
//   - 网络/进程操作全部丢给线程池，结果用 core::kMsgRemoteResult 投回对话框
//     自己（模态循环照常分发消息，与更新对话框同一套）。
// 状态行与"当前目录"都是普通 STATIC：回归脚本用 WM_GETTEXT 逐字读回来核对；
// 列表内容也在对话框的 WM_GETTEXT 里（行用 \r\n 连接，与 XPath 对话框同款——
// 跨进程读标准列表框的 LB_GETTEXT 要传指针，不可靠）。
class RemoteDialog {
public:
    struct Request {
        std::vector<core::RemoteSite> sites;   // 站点表（来自配置中心）
        std::wstring configRoot;               // 下载缓存根（config.xml 所在目录）
        std::wstring currentFilePath;          // 当前文档的本地路径（空 = 没打开文件）
    };

    struct Outcome {
        std::wstring openLocalPath;            // 非空 = 把这个本地缓存文件打开
        std::vector<core::RemoteSite> sites;   // 站点表（可能被站点编辑器改过）
        bool sitesChanged = false;             // true = 调用方要把新表写回配置
    };

    static Outcome Show(HWND owner, infra::ThreadPool& pool, Request request);
};

// 站点编辑器（窗口类名 notepadccRemoteSites）：浏览对话框的「站点设置…」按钮走它，
// 单独弹也行。返回 true 表示改过，新表写进 out；false = 用户没动过。
class RemoteSiteDialog {
public:
    static bool Show(HWND owner, const std::vector<core::RemoteSite>& sites,
                     std::vector<core::RemoteSite>* out);
};

}  // namespace npcc::shell
