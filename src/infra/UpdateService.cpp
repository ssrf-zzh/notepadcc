#include "infra/UpdateService.h"

#include <windows.h>
#include <winhttp.h>

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

std::wstring WinErrorText(const wchar_t* what, unsigned long code) {
    return std::wstring(what) + L"（WinHTTP 错误 " + std::to_wstring(code) + L"）";
}

// 把系统/IE 的代理配置记进日志：开着代理的机器上，"更新检查到底走没走代理"
// 是第一手线索。取不到就什么都不写（不构成错误）。
void LogProxyIfConfigured() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (::WinHttpGetIEProxyConfigForCurrentUser(&config) == 0) {
        return;
    }
    if (config.lpszProxy != nullptr) {
        LogInfo(L"update", std::wstring(L"使用系统代理: ") + config.lpszProxy);
    }
    if (config.lpszProxyBypass != nullptr) {
        LogInfo(L"update", std::wstring(L"代理绕过列表: ") + config.lpszProxyBypass);
    }
    if (config.lpszAutoConfigUrl != nullptr) {
        LogInfo(L"update", std::wstring(L"代理自动配置脚本: ") + config.lpszAutoConfigUrl);
    }
    if (config.lpszProxy != nullptr) {
        ::GlobalFree(config.lpszProxy);
    }
    if (config.lpszProxyBypass != nullptr) {
        ::GlobalFree(config.lpszProxyBypass);
    }
    if (config.lpszAutoConfigUrl != nullptr) {
        ::GlobalFree(config.lpszAutoConfigUrl);
    }
}

struct HttpHandles {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    ~HttpHandles() {
        if (request != nullptr)    ::WinHttpCloseHandle(request);
        if (connection != nullptr) ::WinHttpCloseHandle(connection);
        if (session != nullptr)    ::WinHttpCloseHandle(session);
    }
};

DownloadResult DownloadUrl(const std::string& utf8Url, std::size_t maxBytes) {
    DownloadResult result;
    const std::wstring url = WideFromUtf8(utf8Url);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    components.lpszHostName = host;
    components.dwHostNameLength = 255;
    components.lpszUrlPath = path;
    components.dwUrlPathLength = 2047;
    if (::WinHttpCrackUrl(url.c_str(), 0, 0, &components) == 0) {
        result.error = L"URL 解析失败：" + url;
        return result;
    }
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && components.nScheme != INTERNET_SCHEME_HTTP) {
        result.error = L"只支持 http/https 下载：" + url;
        return result;
    }

    HttpHandles handles;
    // 回环地址绝不过代理：本地回归源与自检都指 127.0.0.1，机器上的代理配置不该插一脚。
    // 其余地址先试"自动代理"（系统/IE 配置，含 PAC/WPAD），失败再退回默认代理设置
    const bool loopback = ::_wcsicmp(host, L"127.0.0.1") == 0 ||
                          ::_wcsicmp(host, L"localhost") == 0;
    handles.session = ::WinHttpOpen(
        kUserAgent, loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles.session == nullptr && !loopback) {
        handles.session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (handles.session == nullptr) {
        result.error = WinErrorText(L"WinHTTP 会话创建失败", ::GetLastError());
        return result;
    }
    ::WinHttpSetTimeouts(handles.session, 10000, 10000, 15000, 30000);
    if (!loopback) {
        LogProxyIfConfigured();
    }

    const wchar_t* pathPart = path[0] != L'\0' ? path : L"/";
    handles.connection = ::WinHttpConnect(handles.session, host, components.nPort, 0);
    if (handles.connection == nullptr) {
        result.error = WinErrorText(L"连接失败", ::GetLastError());
        return result;
    }
    handles.request = ::WinHttpOpenRequest(
        handles.connection, L"GET", pathPart, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (handles.request == nullptr) {
        result.error = WinErrorText(L"请求创建失败", ::GetLastError());
        return result;
    }
    // 默认就是"禁止 https 降级到 http"；显式写出来，验证时不用去查默认值
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    ::WinHttpSetOption(handles.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                       sizeof(redirectPolicy));

    if (::WinHttpSendRequest(handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == 0 ||
        ::WinHttpReceiveResponse(handles.request, nullptr) == 0) {
        result.error = WinErrorText(L"请求发送失败", ::GetLastError());
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (::WinHttpQueryHeaders(handles.request,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                              WINHTTP_NO_HEADER_INDEX) == 0) {
        result.error = WinErrorText(L"读不到 HTTP 状态码", ::GetLastError());
        return result;
    }
    if (statusCode != 200) {
        result.error = L"服务器返回 HTTP " + std::to_wstring(statusCode) + L"：" + url;
        return result;
    }

    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(handles.request, &available) == 0) {
            result.error = WinErrorText(L"读数据失败", ::GetLastError());
            return result;
        }
        if (available == 0) {
            break;   // 收完
        }
        const std::size_t offset = result.bytes.size();
        if (offset + available > maxBytes) {
            result.error = L"内容超过大小上限（" + std::to_wstring(maxBytes) + L" 字节）：" + url;
            return result;
        }
        result.bytes.resize(offset + available);
        DWORD read = 0;
        if (::WinHttpReadData(handles.request, result.bytes.data() + offset, available, &read) == 0) {
            result.error = WinErrorText(L"读数据失败", ::GetLastError());
            return result;
        }
        result.bytes.resize(offset + read);
    }
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

std::string Utf8FromWide(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          size, nullptr, nullptr);
    return out;
}

std::wstring WideFromUtf8(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          size);
    return out;
}

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
