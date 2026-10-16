#pragma once

#include <cstdint>
#include <string>

namespace npcc::core {

// 远程站点（M3 第十一块：远程编辑 FTP/SFTP）。
//
// 站点是"连接参数 + 起始目录"的命名配置，与工具栏项表同一个做法存进
// config.xml（数量可变的结构 → 一组子元素，见 ConfigCenter 的 <remote> 节）。
// 密码明文存配置——多数编辑器都这个待遇（SSH 私钥记的是文件路径，不是密码），
// 界面里如实说明，不假装它安全。
enum class RemoteProtocol {
    Ftp,    // 明文 FTP（PASV；协议本身就不过加密链路，界面里写明）
    Sftp,   // 走系统自带 OpenSSH 的 sftp.exe（批处理模式、密钥认证）
};

const wchar_t* RemoteProtocolToken(RemoteProtocol protocol);   // "ftp" / "sftp"
bool ParseRemoteProtocol(const std::wstring& token, RemoteProtocol* out);

struct RemoteSite {
    std::wstring   name;                      // 站点名（也是本地缓存目录的一段）
    RemoteProtocol protocol = RemoteProtocol::Ftp;
    std::wstring   host;
    std::uint16_t  port = 0;                  // 0 = 用协议默认端口（21 / 22）
    std::wstring   user;                      // FTP 留空按 anonymous 处理；SFTP 必填
    std::wstring   password;                  // FTP 用；SFTP 不存密码（密钥认证）
    std::wstring   keyFile;                   // 仅 SFTP：私钥文件（空 = 用 agent / 默认私钥）
    bool           acceptNewHostKey = false;  // 仅 SFTP：StrictHostKeyChecking=accept-new
    std::wstring   initialDir;                // 起始目录（空 = 登录目录）
};

std::uint16_t RemoteDefaultPort(RemoteProtocol protocol);

// 校验站点参数，失败时 error 是给用户看的一句话。
// 校验是纯的（不碰文件系统）：私钥文件在不在，连接时再报。
bool ValidateRemoteSite(const RemoteSite& site, std::wstring* error);

// —— 远程路径：POSIX 风格（"/" 分隔），与本地 Windows 路径分开、不混用 ——
// 全部是纯函数，单测逐条钉住：
std::wstring RemoteNormalizeDir(const std::wstring& dir);
// 结果以 "/" 开头、不以 "/" 结尾（根就是 "/"）；折叠 "//"、"." 与 ".."
// （".." 已在根就不再往上——远端根之上没有"上一级"）。
std::wstring RemoteJoin(const std::wstring& dir, const std::wstring& name);
std::wstring RemoteParentDir(const std::wstring& dir);
std::wstring RemoteLeafName(const std::wstring& path);

// 单个名字段（远程操作的输入：新建文件夹 / 重命名）：非空、不是 "." / ".."、
// 不含 '/' '\\' 与控制字符。
bool RemoteIsValidComponent(const std::wstring& segment);
// 目录路径整体检查：每一段都合法（空路径视为合法 = 登录目录）。
bool RemoteIsValidDirPath(const std::wstring& dir);

// 本地缓存用的一段名字（比上面更严：它最终要变成 Windows 上的一段路径）：
// 再排除 Windows 非法字符 < > : " | ? *、结尾的 '.' / ' '，以及设备名
// （CON / PRN / AUX / NUL / COM1-9 / LPT1-9）——远程文件叫这些名字时，
// 报一句"这个文件名在本地缓存里映射不过来"，而不是猜一个名字。
bool RemoteIsCacheSafeSegment(const std::wstring& segment);

// 下载缓存路径：<配置根>/remote/<站点名>/<远程路径的每一段>。
// 每一段都要过 RemoteIsCacheSafeSegment（远程端是半可信输入，".." 之类
// 绝不能把文件写到配置目录外面去）。失败时 error 说明是哪一段不行。
bool RemoteCachePath(const std::wstring& configRoot, const std::wstring& siteName,
                     const std::wstring& remoteFilePath, std::wstring* out,
                     std::wstring* error);

// 反向：本地缓存路径 → 远程路径（"上传当前文档"要知道它从哪儿来）。
// 只认 <配置根>/remote/<站点名>/ 这个前缀（按段比对，大小写不敏感——Windows
// 路径本来就不分大小写），前缀之外每一段都要能当远程名字用（过
// RemoteIsCacheSafeSegment，"." / ".." 在这种检查里本来就不合法）。
// 前缀对不上、或剩下的段为空/不合法时返回 false（调用方如实说"这个文档不在
// 远程缓存里"，而不是猜一个远程路径）。
bool RemotePathFromCache(const std::wstring& configRoot, const std::wstring& siteName,
                         const std::wstring& localFilePath, std::wstring* out);

}  // namespace npcc::core
