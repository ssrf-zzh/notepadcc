#include "infra/RemoteService.h"

#include <windows.h>

#include <filesystem>
#include <utility>
#include <vector>

#include "infra/Log.h"
#include "infra/UpdateService.h"   // Utf8FromWide / WideFromUtf8

namespace npcc::infra {
namespace {

RemoteResult Fail(std::wstring message) {
    RemoteResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

RemoteResult Ok() {
    RemoteResult result;
    result.ok = true;
    return result;
}

RemoteResult FromSftp(const SftpRunResult& run) {
    RemoteResult result;
    result.ok = run.ok;
    result.error = run.error;
    return result;
}

// 失败统一记一笔日志（回归脚本按日志判成败，界面按 error 说话）。
RemoteResult Finish(RemoteResult result, const core::RemoteSite& site) {
    if (!result.ok) {
        LogWarn(L"remote", L"站点 \u300c" + site.name + L"\u300d操作失败：" + result.error);
    }
    return result;
}

// 一条 SFTP 批处理命令：动词（字母词，可带开关）+ 宽字符参数 → UTF-8 引号形式。
bool AppendSftpLine(std::string* batch, const std::string& verb,
                    const std::vector<std::wstring>& args, std::wstring* error) {
    std::vector<std::string> converted;
    converted.reserve(args.size());
    for (const std::wstring& arg : args) {
        converted.push_back(Utf8FromWide(arg));
    }
    std::string line;
    if (!core::BuildSftpCommand(verb, converted, &line)) {
        *error = L"名字里含双引号或控制字符，SFTP 批处理发不过去（换 FTP 通道，或先改名）";
        return false;
    }
    *batch += line;
    return true;
}

// 本地缓存文件拆成"目录 + 文件名"，并把目录建出来（下载/上传都要它存在）。
bool PrepareLocalFile(const std::wstring& localFile, std::wstring* dir, std::wstring* leaf,
                      std::wstring* error) {
    const std::filesystem::path path(localFile);
    *dir = path.parent_path().wstring();
    *leaf = path.filename().wstring();
    if (dir->empty() || leaf->empty()) {
        *error = L"本地缓存路径不完整：" + localFile;
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(*dir, code);
    if (code) {
        *error = L"创建本地缓存目录失败：" + *dir;
        return false;
    }
    return true;
}

// sftp 的 `ls -l` 输出：混着 "sftp> ls -l /path" 之类的噪声行，认不出来的直接跳过。
RemoteResult ParseSftpListOutput(const std::string& output) {
    RemoteResult result = Ok();
    std::size_t index = 0;
    while (index <= output.size()) {
        const std::size_t end = output.find('\n', index);
        const std::string line = end == std::string::npos ? output.substr(index)
                                                          : output.substr(index, end - index);
        core::FtpListEntry parsed;
        if (core::ParseFtpListLine(line, &parsed)) {
            if (parsed.name != "." && parsed.name != "..") {
                RemoteEntry entry;
                entry.name = WideFromUtf8(parsed.name);
                entry.size = parsed.size;
                entry.isDir = parsed.isDir;
                result.entries.push_back(std::move(entry));
            }
        }
        if (end == std::string::npos) {
            break;
        }
        index = end + 1;
    }
    return result;
}

SftpRunResult RunSftpOp(const core::RemoteSite& site, const std::string& batch,
                        const std::wstring& workingDir, const RemoteServiceOptions& options) {
    return RunSftp(site, batch, workingDir, options.timeoutMs);
}

}  // namespace

std::wstring RemoteInitialDir(const core::RemoteSite& site, const RemoteServiceOptions& options) {
    if (!site.initialDir.empty()) {
        return core::RemoteNormalizeDir(site.initialDir);
    }
    if (site.protocol != core::RemoteProtocol::Sftp) {
        return L"/";
    }
    // SFTP 的 "/" 是远端文件系统根，默认从登录目录开始
    std::string batch;
    std::wstring error;
    if (!AppendSftpLine(&batch, "pwd", {}, &error)) {
        return L"/";
    }
    const SftpRunResult run = RunSftpOp(site, batch, L"", options);
    std::string dir;
    if (!run.ok || !core::ParseSftpPwdOutput(run.output, &dir)) {
        LogInfo(L"remote", L"取 SFTP 登录目录失败，从 \"/\" 开始");
        return L"/";
    }
    return core::RemoteNormalizeDir(WideFromUtf8(dir));
}

RemoteResult RemoteListDir(const core::RemoteSite& site, const std::wstring& dir,
                           const RemoteServiceOptions& options) {
    const std::wstring normalized = core::RemoteNormalizeDir(dir);
    if (site.protocol == core::RemoteProtocol::Ftp) {
        return Finish(FtpList(site, normalized, FtpOptions{options.timeoutMs}), site);
    }
    std::string batch;
    std::wstring error;
    if (!AppendSftpLine(&batch, "ls -l", {normalized}, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    const SftpRunResult run = RunSftpOp(site, batch, L"", options);
    if (!run.ok) {
        return Finish(FromSftp(run), site);
    }
    return ParseSftpListOutput(run.output);
}

RemoteResult RemoteDownloadFile(const core::RemoteSite& site, const std::wstring& remoteFile,
                                const std::wstring& localFile,
                                const RemoteServiceOptions& options) {
    std::wstring dir;
    std::wstring leaf;
    std::wstring error;
    if (!PrepareLocalFile(localFile, &dir, &leaf, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    if (site.protocol == core::RemoteProtocol::Ftp) {
        return Finish(FtpDownload(site, remoteFile, localFile, FtpOptions{options.timeoutMs}),
                      site);
    }
    std::string batch;
    if (!AppendSftpLine(&batch, "lcd", {dir}, &error) ||
        !AppendSftpLine(&batch, "get", {remoteFile, leaf}, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    const SftpRunResult run = RunSftpOp(site, batch, dir, options);
    if (!run.ok) {
        ::DeleteFileW(localFile.c_str());   // 半截缓存不留在盘上
        return Finish(FromSftp(run), site);
    }
    return Ok();
}

RemoteResult RemoteUploadFile(const core::RemoteSite& site, const std::wstring& localFile,
                              const std::wstring& remoteFile,
                              const RemoteServiceOptions& options) {
    if (site.protocol == core::RemoteProtocol::Ftp) {
        return Finish(FtpUpload(site, localFile, remoteFile, FtpOptions{options.timeoutMs}), site);
    }
    std::wstring dir;
    std::wstring leaf;
    std::wstring error;
    if (!PrepareLocalFile(localFile, &dir, &leaf, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    std::string batch;
    if (!AppendSftpLine(&batch, "lcd", {dir}, &error) ||
        !AppendSftpLine(&batch, "put", {leaf, remoteFile}, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    return Finish(FromSftp(RunSftpOp(site, batch, dir, options)), site);
}

RemoteResult RemoteMakeDir(const core::RemoteSite& site, const std::wstring& remoteDir,
                           const RemoteServiceOptions& options) {
    const std::wstring normalized = core::RemoteNormalizeDir(remoteDir);
    if (site.protocol == core::RemoteProtocol::Ftp) {
        return Finish(FtpMkdir(site, normalized, FtpOptions{options.timeoutMs}), site);
    }
    std::string batch;
    std::wstring error;
    if (!AppendSftpLine(&batch, "mkdir", {normalized}, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    return Finish(FromSftp(RunSftpOp(site, batch, L"", options)), site);
}

RemoteResult RemoteDeleteFile(const core::RemoteSite& site, const std::wstring& remoteFile,
                              const RemoteServiceOptions& options) {
    if (site.protocol == core::RemoteProtocol::Ftp) {
        return Finish(FtpRemoveFile(site, remoteFile, FtpOptions{options.timeoutMs}), site);
    }
    std::string batch;
    std::wstring error;
    if (!AppendSftpLine(&batch, "rm", {remoteFile}, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    return Finish(FromSftp(RunSftpOp(site, batch, L"", options)), site);
}

RemoteResult RemoteDeleteDir(const core::RemoteSite& site, const std::wstring& remoteDir,
                             const RemoteServiceOptions& options) {
    const std::wstring normalized = core::RemoteNormalizeDir(remoteDir);
    if (site.protocol == core::RemoteProtocol::Ftp) {
        return Finish(FtpRemoveDir(site, normalized, FtpOptions{options.timeoutMs}), site);
    }
    std::string batch;
    std::wstring error;
    if (!AppendSftpLine(&batch, "rmdir", {normalized}, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    return Finish(FromSftp(RunSftpOp(site, batch, L"", options)), site);
}

RemoteResult RemoteRenamePath(const core::RemoteSite& site, const std::wstring& from,
                              const std::wstring& to, const RemoteServiceOptions& options) {
    if (site.protocol == core::RemoteProtocol::Ftp) {
        return Finish(FtpRename(site, from, to, FtpOptions{options.timeoutMs}), site);
    }
    std::string batch;
    std::wstring error;
    if (!AppendSftpLine(&batch, "rename", {from, to}, &error)) {
        return Finish(Fail(std::move(error)), site);
    }
    return Finish(FromSftp(RunSftpOp(site, batch, L"", options)), site);
}

}  // namespace npcc::infra
