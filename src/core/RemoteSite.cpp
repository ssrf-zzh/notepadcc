#include "core/RemoteSite.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace npcc::core {
namespace {

const std::vector<std::wstring>& ReservedDeviceNames() {
    static const std::vector<std::wstring> names = {
        L"con",  L"prn",  L"aux",  L"nul",  L"com1", L"com2", L"com3", L"com4", L"com5",
        L"com6", L"com7", L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5",
        L"lpt6", L"lpt7", L"lpt8", L"lpt9",
    };
    return names;
}

std::vector<std::wstring> SplitPath(std::wstring_view path) {
    std::vector<std::wstring> out;
    std::wstring current;
    for (const wchar_t ch : path) {
        if (ch == L'/') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    out.push_back(current);
    return out;
}

std::wstring ToLowerAscii(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return text;
}

bool HasControlChar(const std::wstring& text) {
    for (const wchar_t ch : text) {
        if (static_cast<unsigned>(ch) < 0x20u || ch == 0x7F) {
            return true;
        }
    }
    return false;
}

}  // namespace

const wchar_t* RemoteProtocolToken(const RemoteProtocol protocol) {
    return protocol == RemoteProtocol::Sftp ? L"sftp" : L"ftp";
}

bool ParseRemoteProtocol(const std::wstring& token, RemoteProtocol* out) {
    const std::wstring lower = ToLowerAscii(token);
    if (lower == L"ftp") {
        *out = RemoteProtocol::Ftp;
        return true;
    }
    if (lower == L"sftp") {
        *out = RemoteProtocol::Sftp;
        return true;
    }
    return false;
}

std::uint16_t RemoteDefaultPort(const RemoteProtocol protocol) {
    return protocol == RemoteProtocol::Sftp ? 22u : 21u;
}

bool RemoteIsValidComponent(const std::wstring& segment) {
    if (segment.empty() || segment == L"." || segment == L"..") {
        return false;
    }
    if (HasControlChar(segment)) {
        return false;
    }
    return segment.find(L'/') == std::wstring::npos &&
           segment.find(L'\\') == std::wstring::npos;
}

bool RemoteIsValidDirPath(const std::wstring& dir) {
    for (const std::wstring& segment : SplitPath(dir)) {
        if (segment.empty() || segment == L"." || segment == L"..") {
            continue;   // 结构字符（分隔 / 本级 / 上级）由规范化处理
        }
        if (!RemoteIsValidComponent(segment)) {
            return false;
        }
    }
    return true;
}

bool RemoteIsCacheSafeSegment(const std::wstring& segment) {
    if (!RemoteIsValidComponent(segment)) {
        return false;
    }
    if (segment.back() == L'.' || segment.back() == L' ') {
        return false;
    }
    for (const wchar_t ch : segment) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'|' ||
            ch == L'?' || ch == L'*') {
            return false;
        }
    }
    // 设备名：以第一个 '.' 之前的部分比对（CON.txt 在 Windows 上同样是保留名）
    const std::size_t dot = segment.find(L'.');
    const std::wstring base = ToLowerAscii(segment.substr(0, dot));
    const std::vector<std::wstring>& reserved = ReservedDeviceNames();
    return std::find(reserved.begin(), reserved.end(), base) == reserved.end();
}

std::wstring RemoteNormalizeDir(const std::wstring& dir) {
    std::vector<std::wstring> segments;
    for (const std::wstring& raw : SplitPath(dir)) {
        if (raw.empty() || raw == L".") {
            continue;
        }
        if (raw == L"..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
            continue;
        }
        segments.push_back(raw);
    }
    std::wstring out = L"/";
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index != 0) {
            out += L'/';
        }
        out += segments[index];
    }
    return out;
}

std::wstring RemoteJoin(const std::wstring& dir, const std::wstring& name) {
    if (name.empty()) {
        return RemoteNormalizeDir(dir);
    }
    const std::wstring base = RemoteNormalizeDir(dir);
    return RemoteNormalizeDir(base == L"/" ? L"/" + name : base + L"/" + name);
}

std::wstring RemoteParentDir(const std::wstring& dir) {
    const std::wstring normalized = RemoteNormalizeDir(dir);
    if (normalized == L"/") {
        return L"/";
    }
    const std::size_t slash = normalized.find_last_of(L'/');
    return slash == 0 ? std::wstring(L"/") : normalized.substr(0, slash);
}

std::wstring RemoteLeafName(const std::wstring& path) {
    const std::wstring normalized = RemoteNormalizeDir(path);
    const std::size_t slash = normalized.find_last_of(L'/');
    return slash == std::wstring::npos ? normalized : normalized.substr(slash + 1);
}

bool ValidateRemoteSite(const RemoteSite& site, std::wstring* error) {
    const auto fail = [error](const wchar_t* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (site.name.empty()) {
        return fail(L"站点名不能为空");
    }
    if (!RemoteIsCacheSafeSegment(site.name)) {
        return fail(L"站点名不能含 / \\ 与控制字符，也不能是 Windows 上的非法文件名"
                    L"（它要当本地缓存目录的一段用）");
    }
    if (site.host.empty()) {
        return fail(L"主机名不能为空");
    }
    if (HasControlChar(site.host) || site.host.find(L' ') != std::wstring::npos ||
        site.host.find(L'/') != std::wstring::npos ||
        site.host.find(L'\\') != std::wstring::npos ||
        site.host.find(L'"') != std::wstring::npos) {
        return fail(L"主机名不能含空格、引号、斜杠或控制字符");
    }
    if (site.protocol == RemoteProtocol::Sftp && site.user.empty()) {
        return fail(L"SFTP 需要用户名（调用系统 sftp.exe 的命令行形式是 user@host）");
    }
    if (HasControlChar(site.user) || site.user.find(L'"') != std::wstring::npos ||
        site.user.find(L'@') != std::wstring::npos) {
        return fail(L"用户名不能含 @、引号或控制字符");
    }
    if (HasControlChar(site.password)) {
        return fail(L"密码不能含控制字符");
    }
    if (HasControlChar(site.keyFile)) {
        return fail(L"私钥文件路径不能含控制字符");
    }
    if (HasControlChar(site.initialDir)) {
        return fail(L"起始目录不能含控制字符");
    }
    return true;
}

bool RemoteCachePath(const std::wstring& configRoot, const std::wstring& siteName,
                     const std::wstring& remoteFilePath, std::wstring* out,
                     std::wstring* error) {
    const auto fail = [error](std::wstring message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    if (!RemoteIsCacheSafeSegment(siteName)) {
        return fail(L"站点名 \"" + siteName + L"\" 不能用作本地目录名");
    }
    std::wstring path = configRoot;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path += L'\\';
    }
    path += L"remote\\";
    path += siteName;
    for (const std::wstring& segment : SplitPath(remoteFilePath)) {
        if (segment.empty() || segment == L".") {
            continue;
        }
        if (segment == L"..") {
            return fail(L"远程路径里含 \"..\"，拒绝映射到本地缓存");
        }
        if (!RemoteIsCacheSafeSegment(segment)) {
            return fail(L"远程名字 \"" + segment +
                        L"\" 在本地缓存里映射不过来（含 Windows 非法字符或保留名）");
        }
        path += L'\\';
        path += segment;
    }
    if (out != nullptr) {
        *out = std::move(path);
    }
    return true;
}

namespace {

// 本地路径按 "\" 与 "/" 都拆（配置根可能来自环境变量或用户输入，两种混着来）
std::vector<std::wstring> SplitLocalPath(const std::wstring& path) {
    std::vector<std::wstring> out;
    std::wstring current;
    for (const wchar_t ch : path) {
        if (ch == L'\\' || ch == L'/') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

}  // namespace

bool RemotePathFromCache(const std::wstring& configRoot, const std::wstring& siteName,
                         const std::wstring& localFilePath, std::wstring* out) {
    if (!RemoteIsCacheSafeSegment(siteName) || configRoot.empty() || localFilePath.empty()) {
        return false;
    }
    std::vector<std::wstring> prefix = SplitLocalPath(configRoot);
    prefix.push_back(L"remote");
    prefix.push_back(siteName);
    const std::vector<std::wstring> segments = SplitLocalPath(localFilePath);
    if (segments.size() <= prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (ToLowerAscii(segments[index]) != ToLowerAscii(prefix[index])) {
            return false;
        }
    }
    std::wstring remote = L"/";
    for (std::size_t index = prefix.size(); index < segments.size(); ++index) {
        if (!RemoteIsCacheSafeSegment(segments[index])) {
            return false;
        }
        if (index != prefix.size()) {
            remote += L'/';
        }
        remote += segments[index];
    }
    if (out != nullptr) {
        *out = std::move(remote);
    }
    return true;
}

}  // namespace npcc::core
