#pragma once

#include <string>

#include "core/RemoteSite.h"
#include "infra/FtpClient.h"    // RemoteEntry / RemoteResult（两种协议共用同一套结果）
#include "infra/SftpRunner.h"

namespace npcc::infra {

// 远程编辑的统一门面（M3 第十一块）：上层（主窗口、远程浏览对话框）只认
// 这里的动作，由它按站点协议分发——
//   FTP  → FtpClient（自研 WinSock2 客户端，见 infra/FtpClient.h）
//   SFTP → SftpRunner（系统自带 OpenSSH 的 sftp.exe 批处理，见 infra/SftpRunner.h）
// 结果统一成 RemoteResult（ok / error / entries），界面与回归脚本都对着它写。
struct RemoteServiceOptions {
    int timeoutMs = 15000;   // 单次操作超时上限（连接、收发、等待子进程各按它算）
};

// 站点第一次打开时浏览哪个目录：
//  配了起始目录 → 规范化后直接用；
//  FTP → "/"（FTP 的 "/" 是账号自己的根）；
//  SFTP → 不默认 "/"（那是远端文件系统的根，普通账号多半点不开），
//  先跑一次 `pwd` 取登录目录，拿不到再退回 "/"。
std::wstring RemoteInitialDir(const core::RemoteSite& site,
                              const RemoteServiceOptions& options = {});

RemoteResult RemoteListDir(const core::RemoteSite& site, const std::wstring& dir,
                           const RemoteServiceOptions& options = {});
RemoteResult RemoteDownloadFile(const core::RemoteSite& site, const std::wstring& remoteFile,
                                const std::wstring& localFile,
                                const RemoteServiceOptions& options = {});
RemoteResult RemoteUploadFile(const core::RemoteSite& site, const std::wstring& localFile,
                              const std::wstring& remoteFile,
                              const RemoteServiceOptions& options = {});
RemoteResult RemoteMakeDir(const core::RemoteSite& site, const std::wstring& remoteDir,
                           const RemoteServiceOptions& options = {});
RemoteResult RemoteDeleteFile(const core::RemoteSite& site, const std::wstring& remoteFile,
                              const RemoteServiceOptions& options = {});
RemoteResult RemoteDeleteDir(const core::RemoteSite& site, const std::wstring& remoteDir,
                             const RemoteServiceOptions& options = {});
RemoteResult RemoteRenamePath(const core::RemoteSite& site, const std::wstring& from,
                              const std::wstring& to,
                              const RemoteServiceOptions& options = {});

}  // namespace npcc::infra
