#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/FtpProtocol.h"
#include "core/RemoteSite.h"

namespace npcc::infra {

// 远程操作的结果（列表 / 下载 / 上传 / 目录操作共用）。
struct RemoteEntry {
    std::wstring  name;
    std::uint64_t size = 0;
    bool          isDir = false;
};

struct RemoteResult {
    bool                     ok = false;
    std::wstring             error;     // 失败时给用户看的一句话（尽量带 FTP 应答原文）
    std::vector<RemoteEntry> entries;   // 只有 List 用
};

// 自研的最小 FTP 客户端（M3 第十一块）。只做远程编辑用得上的事：
// 登录、列目录、下载、上传、建目录、改名、删除；只走 PASV、只走二进制
// 模式（TYPE I），不解析厂商私有应答。每个操作自建一次会话
// （连接 → 登录 → 干活 → QUIT）；每一步都有超时上限，调用方在后台线程跑。
struct FtpOptions {
    int timeoutMs = 10000;   // 连接 / 单次收发的超时上限
};

RemoteResult FtpList(const core::RemoteSite& site, const std::wstring& dir,
                     const FtpOptions& options = {});
RemoteResult FtpDownload(const core::RemoteSite& site, const std::wstring& remoteFile,
                         const std::wstring& localFile, const FtpOptions& options = {});
RemoteResult FtpUpload(const core::RemoteSite& site, const std::wstring& localFile,
                       const std::wstring& remoteFile, const FtpOptions& options = {});
RemoteResult FtpMkdir(const core::RemoteSite& site, const std::wstring& remoteDir,
                      const FtpOptions& options = {});
RemoteResult FtpRemoveFile(const core::RemoteSite& site, const std::wstring& remoteFile,
                           const FtpOptions& options = {});
RemoteResult FtpRemoveDir(const core::RemoteSite& site, const std::wstring& remoteDir,
                          const FtpOptions& options = {});
RemoteResult FtpRename(const core::RemoteSite& site, const std::wstring& from,
                       const std::wstring& to, const FtpOptions& options = {});

}  // namespace npcc::infra
