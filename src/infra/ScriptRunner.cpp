#include "infra/ScriptRunner.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <vector>

#include "infra/Paths.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"

namespace npcc::infra {
namespace {

std::atomic<unsigned> gTempCounter{0};

}  // namespace

bool ResolveScriptInterpreter(std::wstring_view configured, core::ScriptInterpreter* out,
                              std::wstring* error) {
    if (out == nullptr) {
        return false;
    }
    const std::vector<core::ScriptInterpreter> candidates =
        core::ScriptInterpreterCandidates(configured);
    for (const core::ScriptInterpreter& candidate : candidates) {
        const bool hasSeparator = candidate.executable.find_first_of(L"\\/") != std::wstring::npos;
        std::wstring resolved = candidate.executable;
        bool usable = false;
        if (hasSeparator) {
            const DWORD attributes = ::GetFileAttributesW(candidate.executable.c_str());
            usable = attributes != INVALID_FILE_ATTRIBUTES &&
                     (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        } else {
            wchar_t found[MAX_PATH] = {};
            const DWORD length = ::SearchPathW(nullptr, candidate.executable.c_str(), nullptr,
                                               MAX_PATH, found, nullptr);
            if (length > 0 && length < MAX_PATH) {
                resolved = found;
                usable = true;
            }
        }
        if (usable) {
            *out = candidate;
            out->executable = std::move(resolved);
            return true;
        }
        if (hasSeparator) {
            // 配置里给了路径但文件不在：直接说清楚，不去 PATH 里另找一个顶替
            if (error != nullptr) {
                *error = L"配置里指定的脚本解释器不存在：\n" + candidate.executable +
                         L"\n（改 config.xml 里 <script interpreter=\"…\"> 这一项，"
                         L"或删掉 interpreter 属性改用 PATH 里的 python.exe）";
            }
            return false;
        }
    }
    if (error != nullptr) {
        *error = L"没有找到 Python 解释器：PATH 里既没有 python.exe 也没有 py.exe。\n"
                 L"装了 Python 的话把安装目录加进 PATH，或在 config.xml 的 <script> 节里写上"
                 L" interpreter=\"C:\\…\\python.exe\"。";
    }
    return false;
}

ScriptRunner::~ScriptRunner() {
    if (process_ != nullptr) {
        ::TerminateProcess(process_, 1);
        ::CloseHandle(process_);
        process_ = nullptr;
    }
    RemoveTempFiles();
}

bool ScriptRunner::Start(const Options& options, Result* result) {
    if (result == nullptr) {
        return false;
    }
    *result = Result{};
    timeoutMs_ = options.timeoutMs;
    killed_ = false;

    std::error_code ignored;
    std::filesystem::create_directories(TempDir(), ignored);
    const std::wstring base = L"script-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                              std::to_wstring(gTempCounter.fetch_add(1));
    inputPath_ = (TempDir() / (base + L".in")).wstring();
    outputPath_ = (TempDir() / (base + L".out")).wstring();
    errorPath_ = (TempDir() / (base + L".err")).wstring();

    // 先把输入写盘：子进程的 stdin 就是这个文件的读句柄
    {
        HANDLE file = ::CreateFileW(inputPath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            result->startError =
                L"建不出喂给脚本的临时输入文件：" + io::DescribeWin32Error(::GetLastError());
            RemoveTempFiles();
            return false;
        }
        const char* data = options.stdinBytes.data();
        std::size_t remaining = options.stdinBytes.size();
        bool writtenAll = true;
        while (remaining > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
            DWORD written = 0;
            if (::WriteFile(file, data, chunk, &written, nullptr) == FALSE || written == 0) {
                writtenAll = false;
                break;
            }
            data += written;
            remaining -= written;
        }
        ::CloseHandle(file);
        if (!writtenAll) {
            result->startError =
                L"写喂给脚本的临时输入文件失败：" + io::DescribeWin32Error(::GetLastError());
            RemoveTempFiles();
            return false;
        }
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;   // 三个标准流句柄要被继承，子进程才拿得到

    auto openForChild = [&security](const std::wstring& path, DWORD access,
                                    DWORD creation) -> HANDLE {
        return ::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    HANDLE inputHandle = openForChild(inputPath_, GENERIC_READ, OPEN_EXISTING);
    HANDLE outputHandle = openForChild(outputPath_, GENERIC_WRITE, CREATE_ALWAYS);
    HANDLE errorHandle = openForChild(errorPath_, GENERIC_WRITE, CREATE_ALWAYS);
    if (inputHandle == INVALID_HANDLE_VALUE || outputHandle == INVALID_HANDLE_VALUE ||
        errorHandle == INVALID_HANDLE_VALUE) {
        if (inputHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(inputHandle);
        }
        if (outputHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(outputHandle);
        }
        if (errorHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(errorHandle);
        }
        result->startError =
            L"建不出脚本的标准流临时文件：" + io::DescribeWin32Error(::GetLastError());
        RemoveTempFiles();
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inputHandle;
    startup.hStdOutput = outputHandle;
    startup.hStdError = errorHandle;
    PROCESS_INFORMATION info{};
    std::wstring line = options.commandLine;   // CreateProcessW 可能要改这块内存
    const BOOL started = ::CreateProcessW(
        nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        options.workingDir.empty() ? nullptr : options.workingDir.c_str(), &startup, &info);
    const DWORD startError = ::GetLastError();
    ::CloseHandle(inputHandle);
    ::CloseHandle(outputHandle);
    ::CloseHandle(errorHandle);
    if (started == FALSE) {
        result->startError = L"启动脚本失败（" + io::DescribeWin32Error(startError) + L"）";
        RemoveTempFiles();
        return false;
    }
    ::CloseHandle(info.hThread);
    process_ = info.hProcess;
    startedTick_ = ::GetTickCount64();
    result->started = true;
    return true;
}

bool ScriptRunner::Exited() const {
    if (process_ == nullptr) {
        return true;
    }
    return ::WaitForSingleObject(process_, 0) == WAIT_OBJECT_0;
}

bool ScriptRunner::TimedOut() const {
    if (process_ == nullptr) {
        return false;
    }
    if (::WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        return false;
    }
    return ::GetTickCount64() - startedTick_ >= timeoutMs_;
}

void ScriptRunner::Kill() {
    if (process_ != nullptr && !killed_) {
        ::TerminateProcess(process_, 1);
        killed_ = true;
    }
}

ScriptRunner::Result ScriptRunner::Finish() {
    Result result;
    result.killed = killed_;
    if (process_ != nullptr) {
        result.started = true;
        ::GetExitCodeProcess(process_, &result.exitCode);
        ::CloseHandle(process_);
        process_ = nullptr;
        result.elapsedMs = ::GetTickCount64() - startedTick_;
    }

    // 上限与运行命令一致：脚本刷屏的时候不该把整份输出搬进内存再搬进文档
    constexpr std::uint64_t kMaxStdout = 8ull * 1024 * 1024;
    constexpr std::uint64_t kMaxStderr = 1ull * 1024 * 1024;
    const io::ReadResult out = io::ReadWholeFile(outputPath_, kMaxStdout);
    if (out.ok) {
        result.stdoutBytes = out.bytes;
    } else if (out.tooLarge) {
        result.stdoutTooLarge = true;
    }
    const io::ReadResult err = io::ReadWholeFile(errorPath_, kMaxStderr);
    if (err.ok) {
        result.stderrBytes = err.bytes;
    } else if (err.tooLarge) {
        result.stderrTooLarge = true;
    }

    RemoveTempFiles();
    return result;
}

void ScriptRunner::RemoveTempFiles() {
    if (!inputPath_.empty()) {
        ::DeleteFileW(inputPath_.c_str());
    }
    if (!outputPath_.empty()) {
        ::DeleteFileW(outputPath_.c_str());
    }
    if (!errorPath_.empty()) {
        ::DeleteFileW(errorPath_.c_str());
    }
    inputPath_.clear();
    outputPath_.clear();
    errorPath_.clear();
}

}  // namespace npcc::infra
