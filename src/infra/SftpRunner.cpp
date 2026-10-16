#include "infra/SftpRunner.h"

#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

#include "infra/Log.h"
#include "infra/UpdateService.h"   // Utf8FromWide / WideFromUtf8

namespace npcc::infra {
namespace {

std::wstring SftpExePathImpl() {
    wchar_t systemDir[MAX_PATH] = {};
    if (::GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
        return {};
    }
    std::wstring path = systemDir;
    path += L"\\OpenSSH\\sftp.exe";
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }
    return path;
}

std::wstring QuoteArg(const std::wstring& text) {
    // 命令行参数的最小引号规则：包一层双引号，内部的双引号按 Windows 规则
    // 反斜杠转义。站点校验已经把双引号挡在外面，这里是第二道。
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : text) {
        if (ch == L'\\') {
            ++backslashes;
            out.push_back(ch);
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring TrimForError(const std::string& text) {
    std::wstring wide = WideFromUtf8(text);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' ||
                             wide.back() == L' ' || wide.back() == L'\t')) {
        wide.pop_back();
    }
    constexpr std::size_t kMax = 400;
    if (wide.size() > kMax) {
        wide.resize(kMax);
        wide += L"…";
    }
    return wide;
}

class Pipe {
public:
    ~Pipe() { Close(); }
    HANDLE ReadEnd() const { return read_; }
    HANDLE WriteEnd() const { return write_; }

    static bool Create(Pipe* readPipe, Pipe* writePipe) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE read = nullptr;
        HANDLE write = nullptr;
        if (!::CreatePipe(&read, &write, &attributes, 0)) {
            return false;
        }
        ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        readPipe->read_ = read;
        readPipe->write_ = nullptr;
        writePipe->read_ = nullptr;
        writePipe->write_ = write;
        return true;
    }

    void Close() {
        if (read_ != nullptr) {
            ::CloseHandle(read_);
            read_ = nullptr;
        }
        if (write_ != nullptr) {
            ::CloseHandle(write_);
            write_ = nullptr;
        }
    }

private:
    HANDLE read_ = nullptr;
    HANDLE write_ = nullptr;
};

std::string ReadAllFromPipe(const HANDLE pipe) {
    std::string out;
    char chunk[4096];
    DWORD got = 0;
    while (::ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        out.append(chunk, got);
    }
    return out;
}

}  // namespace

bool SftpAvailable() {
    return !SftpExePathImpl().empty();
}

bool BuildSftpArguments(const core::RemoteSite& site, std::wstring* arguments,
                        std::wstring* error) {
    const auto fail = [error](const std::wstring& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (site.protocol != core::RemoteProtocol::Sftp) {
        return fail(L"不是 SFTP 站点");
    }
    if (site.host.empty() || site.user.empty()) {
        return fail(L"SFTP 需要主机名与用户名");
    }
    std::wstring text = L"-b - -P ";
    text += std::to_wstring(site.port != 0 ? site.port : 22);
    text += L" -oBatchMode=yes -oConnectTimeout=10";
    if (site.acceptNewHostKey) {
        text += L" -oStrictHostKeyChecking=accept-new";
    }
    if (!site.keyFile.empty()) {
        if (::GetFileAttributesW(site.keyFile.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return fail(L"私钥文件不存在：" + site.keyFile);
        }
        text += L" -i ";
        text += QuoteArg(site.keyFile);
    }
    text += L" ";
    text += QuoteArg(site.user + L"@" + site.host);
    if (arguments != nullptr) {
        *arguments = std::move(text);
    }
    return true;
}

SftpRunResult RunSftp(const core::RemoteSite& site, const std::string& batchText,
                      const std::wstring& workingDir, const int timeoutMs) {
    SftpRunResult result;
    const std::wstring exe = SftpExePathImpl();
    if (exe.empty()) {
        result.error = L"系统里没有 sftp.exe（Windows 10 1809+ 的"
                       L"“OpenSSH 客户端”可选功能），SFTP 通道不可用";
        return result;
    }
    std::wstring args;
    std::wstring buildError;
    if (!BuildSftpArguments(site, &args, &buildError)) {
        result.error = std::move(buildError);
        return result;
    }
    std::wstring commandLine = QuoteArg(exe) + L" " + args;

    Pipe stdinPipe;
    Pipe stdinWriter;
    Pipe stdoutPipe;
    Pipe stdoutWriter;
    Pipe stderrPipe;
    Pipe stderrWriter;
    if (!Pipe::Create(&stdinPipe, &stdinWriter) || !Pipe::Create(&stdoutPipe, &stdoutWriter) ||
        !Pipe::Create(&stderrPipe, &stderrWriter)) {
        result.error = L"创建管道失败";
        return result;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = stdinPipe.ReadEnd();
    startup.hStdOutput = stdoutWriter.WriteEnd();
    startup.hStdError = stderrWriter.WriteEnd();

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = ::CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(), &startup, &process);
    if (!created) {
        result.error = L"启动 sftp.exe 失败（错误 " + std::to_wstring(::GetLastError()) + L"）";
        return result;
    }
    // 子进程拿到自己的两端之后，父进程把不属于自己的那几端关掉，
    // 不然读管道永远等不到 EOF。
    stdinPipe.Close();
    stdoutWriter.Close();
    stderrWriter.Close();

    // 两个读线程：stdout 与 stderr 同时收，避免任何一侧管道写满把子进程卡死。
    std::string stdoutText;
    std::string stderrText;
    std::thread outReader([&]() { stdoutText = ReadAllFromPipe(stdoutPipe.ReadEnd()); });
    std::thread errReader([&]() { stderrText = ReadAllFromPipe(stderrPipe.ReadEnd()); });

    bool wrote = true;
    {
        std::size_t sent = 0;
        while (sent < batchText.size()) {
            DWORD written = 0;
            if (!::WriteFile(stdinWriter.WriteEnd(), batchText.data() + sent,
                             static_cast<DWORD>(batchText.size() - sent), &written, nullptr) ||
                written == 0) {
                wrote = false;
                break;
            }
            sent += written;
        }
    }
    stdinWriter.Close();   // 批处理喂完就关：sftp 读到 EOF 就开始干活

    const DWORD waited = ::WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeoutMs));
    bool timedOut = false;
    if (waited == WAIT_TIMEOUT) {
        timedOut = true;
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, 5000);
    }
    outReader.join();
    errReader.join();

    DWORD exitCode = 1;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    result.exitCode = static_cast<int>(exitCode);
    result.output = std::move(stdoutText);
    if (timedOut) {
        result.error = L"sftp.exe 超过 " + std::to_wstring(timeoutMs) + L" 毫秒未结束，已强制结束";
        return result;
    }
    if (!wrote) {
        result.error = L"向 sftp.exe 写批处理命令失败";
        return result;
    }
    if (exitCode != 0) {
        std::wstring detail = TrimForError(stderrText.empty() ? stdoutText : stderrText);
        result.error = L"SFTP 失败（退出码 " + std::to_wstring(exitCode) +
                       (detail.empty() ? L"" : L"）：" + detail + L"）");
        if (detail.empty()) {
            result.error = L"SFTP 失败（退出码 " + std::to_wstring(exitCode) + L"）";
        }
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace npcc::infra
