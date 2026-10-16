#include "infra/FtpClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <initializer_list>
#include <utility>

#include "infra/UpdateService.h"   // Utf8FromWide / WideFromUtf8

namespace npcc::infra {
namespace {

void EnsureWinsock() {
    static const bool ready = []() {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)ready;
}

std::wstring SocketErrorText(const int code) {
    switch (code) {
        case WSAETIMEDOUT:
            return L"网络超时";
        case WSAECONNREFUSED:
            return L"连接被拒绝（服务器没在听这个端口？）";
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
            return L"网络不可达（检查主机名与网络）";
        case WSAHOST_NOT_FOUND:
            return L"主机名解析失败";
        default:
            return L"网络错误 " + std::to_wstring(code);
    }
}

class Socket {
public:
    Socket() = default;
    explicit Socket(const SOCKET handle) : handle_(handle) {}
    ~Socket() { Close(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_SOCKET; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = other.handle_;
            other.handle_ = INVALID_SOCKET;
        }
        return *this;
    }

    SOCKET Get() const { return handle_; }
    bool   Valid() const { return handle_ != INVALID_SOCKET; }

    SOCKET Release() {
        const SOCKET out = handle_;
        handle_ = INVALID_SOCKET;
        return out;
    }

    void Close() {
        if (handle_ != INVALID_SOCKET) {
            ::closesocket(handle_);
            handle_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET handle_ = INVALID_SOCKET;
};

// 带超时的 TCP 连接：非阻塞 connect + select，成功后恢复阻塞并设收发超时。
Socket ConnectTcp(const std::string& host, const std::uint16_t port, const int timeoutMs,
                  std::wstring* error) {
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const std::wstring wideHost = WideFromUtf8(host);
    const std::wstring portText = std::to_wstring(port);
    addrinfoW* results = nullptr;
    if (::GetAddrInfoW(wideHost.c_str(), portText.c_str(), &hints, &results) != 0) {
        *error = L"主机名解析失败：" + wideHost;
        return {};
    }
    Socket best;
    int lastError = 0;
    for (addrinfoW* entry = results; entry != nullptr; entry = entry->ai_next) {
        Socket candidate(::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol));
        if (!candidate.Valid()) {
            lastError = ::WSAGetLastError();
            continue;
        }
        u_long nonBlocking = 1;
        ::ioctlsocket(candidate.Get(), FIONBIO, &nonBlocking);
        const int rc = ::connect(candidate.Get(), entry->ai_addr,
                                 static_cast<int>(entry->ai_addrlen));
        bool connected = rc == 0;
        if (!connected && ::WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(candidate.Get(), &writable);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            const int selected = ::select(0, nullptr, &writable, nullptr, &tv);
            if (selected > 0) {
                int soError = 0;
                int length = sizeof(soError);
                ::getsockopt(candidate.Get(), SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&soError), &length);
                connected = soError == 0;
                lastError = soError;
            } else if (selected == 0) {
                lastError = WSAETIMEDOUT;
            } else {
                lastError = ::WSAGetLastError();
            }
        } else if (!connected) {
            lastError = ::WSAGetLastError();
        }
        if (connected) {
            nonBlocking = 0;
            ::ioctlsocket(candidate.Get(), FIONBIO, &nonBlocking);
            const DWORD timeout = static_cast<DWORD>(timeoutMs);
            ::setsockopt(candidate.Get(), SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            ::setsockopt(candidate.Get(), SOL_SOCKET, SO_SNDTIMEO,
                         reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            best = std::move(candidate);
            break;
        }
    }
    ::FreeAddrInfoW(results);
    if (!best.Valid()) {
        *error = SocketErrorText(lastError);
    }
    return best;
}

constexpr std::size_t kMaxControlBuffer = 64 * 1024;

struct FtpSession {
    core::RemoteSite site;
    int              timeoutMs = 10000;
    Socket           control;
    std::string      buffer;

    bool ReadLine(std::string* line, std::wstring* error) {
        for (;;) {
            const std::size_t end = buffer.find('\n');
            if (end != std::string::npos) {
                *line = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                return true;
            }
            if (buffer.size() > kMaxControlBuffer) {
                *error = L"FTP 应答异常（单行超过 64 KB）";
                return false;
            }
            char chunk[2048];
            const int got = ::recv(control.Get(), chunk, sizeof(chunk), 0);
            if (got <= 0) {
                const int code = ::WSAGetLastError();
                *error = got == 0 ? std::wstring(L"FTP 连接被对方关闭")
                                  : SocketErrorText(code);
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
        }
    }

    bool SendAll(const std::string& text, std::wstring* error) {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const int rc = ::send(control.Get(), text.data() + sent,
                                  static_cast<int>(text.size() - sent), 0);
            if (rc <= 0) {
                *error = SocketErrorText(::WSAGetLastError());
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    bool ReadReply(core::FtpReply* reply, std::wstring* error) {
        std::string first;
        if (!ReadLine(&first, error)) {
            return false;
        }
        core::FtpReply head;
        if (!core::ParseFtpReplyLine(first, &head)) {
            *error = L"看不懂 FTP 应答：" + WideFromUtf8(first);
            return false;
        }
        if (first.size() > 3 && first[3] == '-') {
            std::vector<std::string> lines{first};
            while (!core::ParseFtpMultilineReply(lines, head.code, reply)) {
                std::string next;
                if (!ReadLine(&next, error)) {
                    return false;
                }
                lines.push_back(next);
                if (lines.size() > 200) {
                    *error = L"FTP 多行应答没有收尾（超过 200 行）";
                    return false;
                }
            }
            return true;
        }
        *reply = head;
        return true;
    }

    bool Command(const std::string& command, core::FtpReply* reply, std::wstring* error) {
        if (!SendAll(command + "\r\n", error)) {
            return false;
        }
        return ReadReply(reply, error);
    }

    bool Expect(const std::string& command, const std::initializer_list<int> codes,
                core::FtpReply* reply, std::wstring* error) {
        core::FtpReply current;
        if (!Command(command, &current, error)) {
            return false;
        }
        for (const int code : codes) {
            if (current.code == code) {
                if (reply != nullptr) {
                    *reply = current;
                }
                return true;
            }
        }
        *error = L"FTP " + std::to_wstring(current.code) + L"：" + WideFromUtf8(current.text);
        return false;
    }

    bool Login(std::wstring* error) {
        core::FtpReply greeting;
        if (!ReadReply(&greeting, error)) {
            return false;
        }
        if (greeting.code == 120) {
            if (!ReadReply(&greeting, error)) {
                return false;
            }
        }
        if (greeting.code != 220) {
            *error = L"FTP 服务器问候异常（" + std::to_wstring(greeting.code) + L"：" +
                     WideFromUtf8(greeting.text) + L"）";
            return false;
        }
        const std::string user =
            Utf8FromWide(site.user.empty() ? std::wstring(L"anonymous") : site.user);
        const std::string password = Utf8FromWide(
            site.user.empty() && site.password.empty() ? std::wstring(L"notepadcc@")
                                                       : site.password);
        core::FtpReply reply;
        if (!Expect("USER " + user, {230, 331}, &reply, error)) {
            return false;
        }
        if (reply.code == 331) {
            if (!Expect("PASS " + password, {230, 202}, nullptr, error)) {
                return false;
            }
        }
        // 二进制模式与 UTF-8 名称：UTF8 是尽力而为（老服务器不认 OPTS，
        // 应答必须读掉，不然下一条命令会读到它的应答）。
        if (!Expect("TYPE I", {200}, nullptr, error)) {
            return false;
        }
        core::FtpReply opts;
        if (Command("OPTS UTF8 ON", &opts, error)) {
            // 认不认都继续
        }
        return true;
    }

    bool ChangeDir(const std::wstring& dir, std::wstring* error) {
        const std::wstring normalized = core::RemoteNormalizeDir(dir);
        const std::string path = Utf8FromWide(normalized == L"/" ? std::wstring(L"/") : normalized);
        return Expect("CWD " + path, {250}, nullptr, error);
    }

    // PASV：服务器给一个数据端口，我们连过去。
    bool OpenData(Socket* data, std::wstring* error) {
        core::FtpReply reply;
        if (!Expect("PASV", {227}, &reply, error)) {
            return false;
        }
        std::string host;
        std::uint16_t port = 0;
        if (!core::ParseFtpPasvReply(reply.text, &host, &port)) {
            *error = L"PASV 应答看不懂：" + WideFromUtf8(reply.text);
            return false;
        }
        Socket socket = ConnectTcp(host, port, timeoutMs, error);
        if (!socket.Valid()) {
            return false;
        }
        *data = std::move(socket);
        return true;
    }

    bool ReadDataToEnd(Socket& data, std::string* out, std::wstring* error) {
        std::string collected;
        char chunk[64 * 1024];
        for (;;) {
            const int got = ::recv(data.Get(), chunk, sizeof(chunk), 0);
            if (got == 0) {
                break;
            }
            if (got < 0) {
                *error = SocketErrorText(::WSAGetLastError());
                return false;
            }
            collected.append(chunk, static_cast<std::size_t>(got));
        }
        if (out != nullptr) {
            *out = std::move(collected);
        }
        return true;
    }

    bool SendFileContents(Socket& data, HANDLE file, std::wstring* error) {
        char chunk[64 * 1024];
        for (;;) {
            DWORD got = 0;
            if (!::ReadFile(file, chunk, sizeof(chunk), &got, nullptr)) {
                *error = L"读本地文件失败（错误 " + std::to_wstring(::GetLastError()) + L"）";
                return false;
            }
            if (got == 0) {
                return true;
            }
            std::size_t sent = 0;
            while (sent < got) {
                const int rc = ::send(data.Get(), chunk + sent,
                                      static_cast<int>(got - sent), 0);
                if (rc <= 0) {
                    *error = SocketErrorText(::WSAGetLastError());
                    return false;
                }
                sent += static_cast<std::size_t>(rc);
            }
        }
    }

    // 所有操作的最后一条：不关心应答，服务器不理也无所谓。
    void Quit() {
        std::wstring ignored;
        core::FtpReply reply;
        Command("QUIT", &reply, &ignored);
    }

    bool Connect(std::wstring* error) {
        EnsureWinsock();
        const std::uint16_t port = site.port != 0 ? site.port : core::RemoteDefaultPort(site.protocol);
        control = ConnectTcp(Utf8FromWide(site.host), port, timeoutMs, error);
        if (!control.Valid()) {
            return false;
        }
        return Login(error);
    }

    bool GuardPath(const std::wstring& path, std::wstring* error) {
        if (path.find(L'\r') != std::wstring::npos ||
            path.find(L'\n') != std::wstring::npos) {
            *error = L"路径里含换行，拒绝发送";
            return false;
        }
        return true;
    }
};

RemoteResult Fail(std::wstring message) {
    RemoteResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

// 会话模板：连接 → 干活（lambda）→ QUIT。任务自己决定把结果放哪。
template <typename Body>
RemoteResult RunSession(const core::RemoteSite& site, const FtpOptions& options, Body&& body) {
    std::wstring error;
    FtpSession session;
    session.site = site;
    session.timeoutMs = options.timeoutMs;
    if (!session.Connect(&error)) {
        return Fail(L"连接失败：" + error);
    }
    RemoteResult result = body(session, &error);
    session.Quit();
    return result;
}

RemoteResult DoList(FtpSession& session, const std::wstring& dir, std::wstring* error) {
    if (!session.ChangeDir(dir, error)) {
        return Fail(*error);
    }
    Socket data;
    if (!session.OpenData(&data, error)) {
        return Fail(*error);
    }
    core::FtpReply reply;
    if (!session.Expect("LIST", {125, 150}, &reply, error)) {
        return Fail(*error);
    }
    std::string listing;
    if (!session.ReadDataToEnd(data, &listing, error)) {
        return Fail(*error);
    }
    data.Close();
    core::FtpReply finish;
    if (!session.ReadReply(&finish, error)) {
        return Fail(*error);
    }
    if (finish.code != 226 && finish.code != 250) {
        return Fail(L"LIST 没有正常收尾（FTP " + std::to_wstring(finish.code) + L"：" +
                    WideFromUtf8(finish.text) + L"）");
    }
    RemoteResult result;
    result.ok = true;
    std::size_t index = 0;
    while (index <= listing.size()) {
        const std::size_t end = listing.find('\n', index);
        const std::string line = end == std::string::npos ? listing.substr(index)
                                                          : listing.substr(index, end - index);
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

RemoteResult DoDownload(FtpSession& session, const std::wstring& remoteFile,
                        const std::wstring& localFile, std::wstring* error) {
    if (!session.GuardPath(remoteFile, error)) {
        return Fail(*error);
    }
    const std::wstring dir = core::RemoteParentDir(remoteFile);
    const std::wstring leaf = core::RemoteLeafName(remoteFile);
    if (leaf.empty()) {
        return Fail(L"远程文件名为空");
    }
    if (!session.ChangeDir(dir, error)) {
        return Fail(*error);
    }
    Socket data;
    if (!session.OpenData(&data, error)) {
        return Fail(*error);
    }
    core::FtpReply reply;
    if (!session.Expect("RETR " + Utf8FromWide(leaf), {125, 150}, &reply, error)) {
        return Fail(*error);
    }
    const HANDLE file = ::CreateFileW(localFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Fail(L"写本地缓存失败（" + std::to_wstring(::GetLastError()) + L"）：" + localFile);
    }
    bool writeOk = true;
    char chunk[64 * 1024];
    for (;;) {
        const int got = ::recv(data.Get(), chunk, sizeof(chunk), 0);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            *error = SocketErrorText(::WSAGetLastError());
            writeOk = false;
            break;
        }
        DWORD written = 0;
        if (!::WriteFile(file, chunk, static_cast<DWORD>(got), &written, nullptr) ||
            written != static_cast<DWORD>(got)) {
            *error = L"写本地缓存失败（错误 " + std::to_wstring(::GetLastError()) + L"）";
            writeOk = false;
            break;
        }
    }
    ::CloseHandle(file);
    if (!writeOk) {
        ::DeleteFileW(localFile.c_str());
        return Fail(*error);
    }
    data.Close();
    core::FtpReply finish;
    if (!session.ReadReply(&finish, error)) {
        ::DeleteFileW(localFile.c_str());
        return Fail(*error);
    }
    if (finish.code != 226 && finish.code != 250) {
        ::DeleteFileW(localFile.c_str());
        return Fail(L"下载没有正常收尾（FTP " + std::to_wstring(finish.code) + L"：" +
                    WideFromUtf8(finish.text) + L"）");
    }
    RemoteResult result;
    result.ok = true;
    return result;
}

RemoteResult DoUpload(FtpSession& session, const std::wstring& localFile,
                      const std::wstring& remoteFile, std::wstring* error) {
    if (!session.GuardPath(remoteFile, error)) {
        return Fail(*error);
    }
    const HANDLE file = ::CreateFileW(localFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Fail(L"读本地文件失败（" + std::to_wstring(::GetLastError()) + L"）：" + localFile);
    }
    const std::wstring dir = core::RemoteParentDir(remoteFile);
    const std::wstring leaf = core::RemoteLeafName(remoteFile);
    if (leaf.empty()) {
        ::CloseHandle(file);
        return Fail(L"远程文件名为空");
    }
    if (!session.ChangeDir(dir, error)) {
        ::CloseHandle(file);
        return Fail(*error);
    }
    Socket data;
    if (!session.OpenData(&data, error)) {
        ::CloseHandle(file);
        return Fail(*error);
    }
    core::FtpReply reply;
    if (!session.Expect("STOR " + Utf8FromWide(leaf), {125, 150}, &reply, error)) {
        ::CloseHandle(file);
        return Fail(*error);
    }
    const bool sent = session.SendFileContents(data, file, error);
    ::CloseHandle(file);
    if (!sent) {
        return Fail(*error);
    }
    data.Close();
    core::FtpReply finish;
    if (!session.ReadReply(&finish, error)) {
        return Fail(*error);
    }
    if (finish.code != 226 && finish.code != 250) {
        return Fail(L"上传没有正常收尾（FTP " + std::to_wstring(finish.code) + L"：" +
                    WideFromUtf8(finish.text) + L"）");
    }
    RemoteResult result;
    result.ok = true;
    return result;
}

RemoteResult DoMkdir(FtpSession& session, const std::wstring& remoteDir, std::wstring* error) {
    if (!session.GuardPath(remoteDir, error)) {
        return Fail(*error);
    }
    if (!session.Expect("MKD " + Utf8FromWide(core::RemoteNormalizeDir(remoteDir)), {250, 257},
                        nullptr, error)) {
        return Fail(*error);
    }
    RemoteResult result;
    result.ok = true;
    return result;
}

RemoteResult DoRemoveFile(FtpSession& session, const std::wstring& remoteFile,
                          std::wstring* error) {
    if (!session.GuardPath(remoteFile, error)) {
        return Fail(*error);
    }
    if (!session.Expect("DELE " + Utf8FromWide(remoteFile), {200, 250}, nullptr, error)) {
        return Fail(*error);
    }
    RemoteResult result;
    result.ok = true;
    return result;
}

RemoteResult DoRemoveDir(FtpSession& session, const std::wstring& remoteDir, std::wstring* error) {
    if (!session.GuardPath(remoteDir, error)) {
        return Fail(*error);
    }
    if (!session.Expect("RMD " + Utf8FromWide(remoteDir), {200, 250}, nullptr, error)) {
        return Fail(*error);
    }
    RemoteResult result;
    result.ok = true;
    return result;
}

RemoteResult DoRename(FtpSession& session, const std::wstring& from, const std::wstring& to,
                      std::wstring* error) {
    if (!session.GuardPath(from, error) || !session.GuardPath(to, error)) {
        return Fail(*error);
    }
    if (!session.Expect("RNFR " + Utf8FromWide(from), {350}, nullptr, error)) {
        return Fail(*error);
    }
    if (!session.Expect("RNTO " + Utf8FromWide(to), {250}, nullptr, error)) {
        return Fail(*error);
    }
    RemoteResult result;
    result.ok = true;
    return result;
}

}  // namespace

RemoteResult FtpList(const core::RemoteSite& site, const std::wstring& dir,
                     const FtpOptions& options) {
    return RunSession(site, options, [&](FtpSession& session, std::wstring* error) {
        return DoList(session, dir, error);
    });
}

RemoteResult FtpDownload(const core::RemoteSite& site, const std::wstring& remoteFile,
                         const std::wstring& localFile, const FtpOptions& options) {
    return RunSession(site, options, [&](FtpSession& session, std::wstring* error) {
        return DoDownload(session, remoteFile, localFile, error);
    });
}

RemoteResult FtpUpload(const core::RemoteSite& site, const std::wstring& localFile,
                       const std::wstring& remoteFile, const FtpOptions& options) {
    return RunSession(site, options, [&](FtpSession& session, std::wstring* error) {
        return DoUpload(session, localFile, remoteFile, error);
    });
}

RemoteResult FtpMkdir(const core::RemoteSite& site, const std::wstring& remoteDir,
                      const FtpOptions& options) {
    return RunSession(site, options, [&](FtpSession& session, std::wstring* error) {
        return DoMkdir(session, remoteDir, error);
    });
}

RemoteResult FtpRemoveFile(const core::RemoteSite& site, const std::wstring& remoteFile,
                           const FtpOptions& options) {
    return RunSession(site, options, [&](FtpSession& session, std::wstring* error) {
        return DoRemoveFile(session, remoteFile, error);
    });
}

RemoteResult FtpRemoveDir(const core::RemoteSite& site, const std::wstring& remoteDir,
                          const FtpOptions& options) {
    return RunSession(site, options, [&](FtpSession& session, std::wstring* error) {
        return DoRemoveDir(session, remoteDir, error);
    });
}

RemoteResult FtpRename(const core::RemoteSite& site, const std::wstring& from,
                       const std::wstring& to, const FtpOptions& options) {
    return RunSession(site, options, [&](FtpSession& session, std::wstring* error) {
        return DoRename(session, from, to, error);
    });
}

}  // namespace npcc::infra
