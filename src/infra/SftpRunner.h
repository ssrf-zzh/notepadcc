#pragma once

#include <string>

#include "core/RemoteSite.h"

namespace npcc::infra {

// SFTP 通道（M3 第十一块）：调用**系统自带**的 OpenSSH 客户端
// （%SystemRoot%\System32\OpenSSH\sftp.exe）的批处理模式（-b -），
// stdin 喂一批命令、stdout 收结果。不链第三方 SSH 库——这是这个项目
// 一贯的取舍：能用系统已有的东西就不引入依赖。
//
// 边界如实写在明面上：
//  - 只支持**密钥认证**（密钥文件 / ssh-agent）。批处理模式下密码认证
//    需要交互终端，做不了——界面上写明；
//  - 主机密钥默认严格校验（用用户自己的 known_hosts）；站点里可以显式
//    勾"首次连接自动接受主机密钥"，等价于 -o StrictHostKeyChecking=accept-new；
//  - 不实现断点续传与递归传输。
struct SftpRunResult {
    bool         ok = false;
    int          exitCode = -1;
    std::string  output;    // stdout 原文（列目录时就是 ls -l 的输出）
    std::wstring error;     // 失败说明（含 stderr 的头部几行）
};

// 系统里有没有 sftp.exe。
bool SftpAvailable();

// 组装 sftp.exe 的参数部分（不含引号包住的程序路径本身；纯函数，单测钉住）。
// 形如：-b - -P 2222 -oBatchMode=yes -oConnectTimeout=10 -oStrictHostKeyChecking=accept-new
//       -i "C:\keys\id_ed25519" tester@host
bool BuildSftpArguments(const core::RemoteSite& site, std::wstring* arguments,
                        std::wstring* error);

// 跑一批 sftp 批处理命令：batchText（UTF-8）从 stdin 进去，工作目录 workingDir
// （批处理里 get/put 的相对路径就落在这里，本地路径因此永远不进命令行——
// 中文目录名不会卡在编码上）。timeoutMs 到了没结束就终止进程、按失败返回。
SftpRunResult RunSftp(const core::RemoteSite& site, const std::string& batchText,
                      const std::wstring& workingDir, int timeoutMs = 30000);

}  // namespace npcc::infra
