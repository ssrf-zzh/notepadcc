#include "infra/UpdateService.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/UpdateKey.h"
#include "core/UpdateManifest.h"
#include "core/UpdateVerify.h"
#include "infra/Crypto.h"
#include "infra/Http.h"
#include "infra/Log.h"
#include "infra/Paths.h"

namespace npcc::infra {
namespace {

constexpr std::size_t kMaxManifestBytes = 1u << 20;      // 1 MB
constexpr std::size_t kMaxSignatureBytes = 64u << 10;    // 64 KB
constexpr std::size_t kMaxPackageBytes = 128u << 20;     // 128 MB
constexpr const wchar_t* kUserAgent = L"notepadcc-update/1.0";

struct DownloadResult {
    bool                        ok = false;
    std::wstring                error;
    std::vector<std::uint8_t>   bytes;
};

DownloadResult DownloadUrl(const std::string& utf8Url, std::size_t maxBytes) {
    DownloadResult result;
    HttpRequest request;
    request.method = "GET";
    request.url = utf8Url;
    request.maxBytes = maxBytes;
    request.timeoutMs = 10000;
    const HttpResponse response = HttpSend(request);
    if (!response.ok) {
        result.error = response.error;
        return result;
    }
    if (response.statusCode != 200) {
        result.error = L"服务器返回 HTTP " + std::to_wstring(response.statusCode) + L"：" +
                       WideFromUtf8(utf8Url);
        return result;
    }
    result.bytes = response.bytes;
    result.ok = true;
    return result;
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                std::wstring* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error != nullptr) {
            *error = L"写不进文件：" + path.wstring();
        }
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        if (error != nullptr) {
            *error = L"写入失败：" + path.wstring();
        }
        return false;
    }
    return true;
}

// 用系统自带的 bsdtar（Windows 10 1803+ 的 System32\tar.exe）解压 zip。
// 固定取 System32 下那一份，避免被 PATH 上恰好同名的程序顶掉。
bool ExtractZip(const std::filesystem::path& zipPath, const std::filesystem::path& dest,
                std::wstring* error) {
    wchar_t systemDir[MAX_PATH] = {};
    if (::GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
        if (error != nullptr) {
            *error = L"取不到 System32 目录";
        }
        return false;
    }
    const std::filesystem::path tarPath = std::filesystem::path(systemDir) / L"tar.exe";
    std::error_code ec;
    if (!std::filesystem::exists(tarPath, ec)) {
        if (error != nullptr) {
            *error = L"系统里没有 tar.exe（Windows 10 1803+ 自带），没法解压更新包";
        }
        return false;
    }
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        if (error != nullptr) {
            *error = L"建不了解压目录：" + dest.wstring();
        }
        return false;
    }

    std::wstring command = L"\"" + tarPath.wstring() + L"\" -xf \"" + zipPath.wstring() +
                           L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;   // CreateProcessW 允许改动命令行缓冲
    if (::CreateProcessW(tarPath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == 0) {
        if (error != nullptr) {
            *error = L"起不来 tar.exe（错误 " + std::to_wstring(::GetLastError()) + L"）";
        }
        return false;
    }
    const DWORD waited = ::WaitForSingleObject(process.hProcess, 120000);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 1);
        if (error != nullptr) {
            *error = L"解压超时（120 秒）";
        }
    }
    DWORD exitCode = 1;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0 || exitCode != 0) {
        if (error != nullptr && waited == WAIT_OBJECT_0) {
            *error = L"tar.exe 解压失败（退出码 " + std::to_wstring(exitCode) + L"）";
        }
        return false;
    }
    return true;
}

std::wstring LowerExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
    return extension;
}

// 包里所有需要过 Authenticode 的文件（exe/dll/sys）。空 = 包不完整，直接拒收。
std::vector<std::filesystem::path> FindBinaries(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator iterator(root, ec), end;
         iterator != end; iterator.increment(ec)) {
        if (ec) {
            break;
        }
        if (!iterator->is_regular_file(ec)) {
            continue;
        }
        const std::wstring extension = LowerExtension(iterator->path());
        if (extension == L".exe" || extension == L".dll" || extension == L".sys") {
            found.push_back(iterator->path());
        }
    }
    return found;
}

UpdateCheckReport MakeError(std::wstring detail) {
    UpdateCheckReport report;
    report.status = UpdateCheckReport::Status::Error;
    report.summary = L"更新检查失败";
    report.detail = std::move(detail);
    return report;
}

}  // namespace

UpdateCheckReport RunUpdateCheck(const UpdateCheckOptions& options) {
    UpdateCheckReport report;
    if (options.sourceUrl.empty()) {
        return MakeError(L"更新源地址是空的（设置 → 首选项 → 高级里可以改）");
    }
    std::string base = options.sourceUrl;
    if (base.back() != '/') {
        base += '/';
    }

    // 源策略先于任何一次网络动作：不合规的地址一个请求都不发
    std::wstring why;
    if (!core::IsAllowedUpdateUrl(base, &why) ||
        !core::IsAllowedUpdateUrl(base + "update.json", &why)) {
        return MakeError(L"更新源不合规：" + why);
    }

    LogInfo(L"update", L"开始检查更新：源 " + WideFromUtf8(base) + L"，当前版本 " +
                           WideFromUtf8(options.currentVersion));
    const DownloadResult manifestBytes =
        DownloadUrl(base + "update.json", kMaxManifestBytes);
    if (!manifestBytes.ok) {
        return MakeError(L"下载 update.json 失败：" + manifestBytes.error);
    }
    const DownloadResult signatureBytes =
        DownloadUrl(base + "update.json.sig", kMaxSignatureBytes);
    if (!signatureBytes.ok) {
        return MakeError(L"下载 update.json.sig 失败：" + signatureBytes.error);
    }

    // 先验签**原始字节**，再谈字段内容：签名字节顺序绝不能反过来
    const std::string signatureText(signatureBytes.bytes.begin(), signatureBytes.bytes.end());
    if (!RsaVerifyPkcs1Sha256(manifestBytes.bytes.data(), manifestBytes.bytes.size(),
                             
signatureText, core::kUpdatePublicKeyBase64, &why)) {
        return MakeError(L"清单签名无效：" + why);
    }

    core::UpdateManifest manifest;
    if (!core::ParseUpdateManifest(
            std::string_view(reinterpret_cast<const char*>(manifestBytes.bytes.data()),
                             manifestBytes.bytes.size()),
            manifest, &why)) {
        return MakeError(L"清单格式错误：" + why);
    }
    if (!core::IsAllowedUpdateUrl(manifest.url, &why)) {
        return MakeError(L"清单里的下载地址不合规：" + why);
    }

    report.version = manifest.version;
    report.notes = manifest.notes;

    bool versionOk = false;
    const bool newer = core::IsNewerVersion(manifest.version, options.currentVersion, &versionOk);
    if (!versionOk) {
        return MakeError(L"版本号不是数字点分格式（清单 \"" + WideFromUtf8(manifest.version) +
                         L"\"，当前 \"" + WideFromUtf8(options.currentVersion) + L"\"）");
    }
    if (!newer) {
        report.status = UpdateCheckReport::Status::UpToDate;
        report.summary = L"已是最新（远端 " + WideFromUtf8(manifest.version) + L"，当前 " +
                         WideFromUtf8(options.currentVersion) + L"）";
        LogInfo(L"update", report.summary);
        return report;
    }

    const DownloadResult packageBytes = DownloadUrl(manifest.url, kMaxPackageBytes);
    if (!packageBytes.ok) {
        return MakeError(L"下载更新包失败：" + packageBytes.error);
    }
    const Sha256Digest digest = Sha256(packageBytes.bytes.data(), packageBytes.bytes.size());
    if (digest.Hex() != manifest.sha256) {
        return MakeError(L"更新包 SHA-256 不匹配（期望 " + WideFromUtf8(manifest.sha256) +
                         L"，实际 " + WideFromUtf8(digest.Hex()) + L"）");
    }

    // 落盘 → 解压 → 逐个可执行文件过 Authenticode。任何一步失败都不给"仍然继续"的旁路
    const std::filesystem::path workDir =
        ConfigRoot() / L"update" / L"downloads" / WideFromUtf8(manifest.version);
    std::error_code ec;
    std::filesystem::remove_all(workDir, ec);   // 清掉同版本残留：被校验的就是这一份
    std::filesystem::create_directories(workDir, ec);
    const std::filesystem::path zipPath = workDir / L"package.zip";
    if (!WriteBytes(zipPath, packageBytes.bytes, &why)) {
        return MakeError(why);
    }
    const std::filesystem::path packageDir = workDir / L"package";
    if (!ExtractZip(zipPath, packageDir, &why)) {
        return MakeError(L"解压更新包失败：" + why);
    }

    const std::vector<std::filesystem::path> binaries = FindBinaries(packageDir);
    if (binaries.empty()) {
        return MakeError(L"更新包里没有 .exe/.dll/.sys 文件（疑似打包错误）");
    }
    for (const std::filesystem::path& binary : binaries) {
        if (!core::VerifyAuthenticode(binary.wstring(), &why)) {
            return MakeError(L"Authenticode 校验失败：" + binary.filename().wstring() + L"：" +
                             why);
        }
    }

    report.status = UpdateCheckReport::Status::Available;
    report.packageDir = packageDir.wstring();
    report.summary = L"发现新版本 " + WideFromUtf8(manifest.version);
    LogInfo(L"update", report.summary + L"（" + std::to_wstring(binaries.size()) +
                           L" 个可执行文件全部通过 Authenticode 校验），已解压到 " +
                           report.packageDir);
    return report;
}

}  // namespace npcc::infra
