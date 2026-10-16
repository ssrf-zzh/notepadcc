#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace npcc::core {

// FTP 协议的小解析器（M3 第十一块）。客户端是自己写的
// （infra/FtpClient），所以协议里最容易写错的几处——多行应答的收尾、
// PASV 的六个数、目录列表行的列——全部做成纯函数在这里钉测试。

// 单行应答："230 登录成功"。文本部分原样保留（UTF-8 按原字节走）。
struct FtpReply {
    int         code = 0;
    std::string text;
};
bool ParseFtpReplyLine(const std::string& line, FtpReply* out);

// 多行应答（如 FEAT 的 "211-Features" ... "211 End"）：
// code 为 0 时从首行推断；返回 false 表示还没读到收尾行（调用方继续读）。
// out->text = 中间各行原文 + 收尾行的消息文本，用 "\n" 连接。
bool ParseFtpMultilineReply(const std::vector<std::string>& lines, int code, FtpReply* out);

// PASV 应答："227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" → host、port。
bool ParseFtpPasvReply(const std::string& text, std::string* host, std::uint16_t* port);

struct FtpListEntry {
    std::string   name;
    std::uint64_t size = 0;
    bool          isDir = false;
};

// 目录列表行 → 条目，吃两种常见格式（FTP 的 LIST 与 sftp 的 ls -l 同吃）：
//   UNIX：-rw-r--r--    1 user group  1234 Jan 10 12:00 name.txt
//   DOS ：01-10-24  12:00PM              1234 name.txt / <DIR>
// 名字取第 8 个空白分隔字段之后的**剩余全部**（名字里可以有空格）。
// 认不出来的行（"total 12"、空行、语法糖）返回 false，调用方跳过。
bool ParseFtpListLine(const std::string& line, FtpListEntry* out);

// sftp 批处理里 "pwd" 的输出（"sftp> pwd" 与 "Remote working directory: X"
// 混着来）：取最后一个 "Remote working directory:" 行，得到远端登录目录。
bool ParseSftpPwdOutput(const std::string& output, std::string* dir);

// SFTP 批处理的一行命令：verb 是 ASCII 字母的词，可带 '-' 开头的开关
// （如 "ls -l"——ls 自己解析开关，引号包住的 "-l" 反而不保险）；
// 每个参数用双引号包住（OpenSSH sftp 的解析器认引号，名字里有空格也能过）。
// 参数里出现双引号或控制字符时返回 false——批处理模式的引号处理各版本
// 不一，宁可不做，由调用方把"这个文件名发不过去"原样报给用户。
bool BuildSftpCommand(const std::string& verb, const std::vector<std::string>& args,
                      std::string* out);

}  // namespace npcc::core
