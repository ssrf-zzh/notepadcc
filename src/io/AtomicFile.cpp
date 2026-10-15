#include "io/AtomicFile.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>

namespace npcc::io {
namespace {

// 临时文件必须与目标同目录：跨卷时 MoveFileEx 会退化成"复制 + 删除"，不再是原子的。
std::wstring TempPathFor(const std::wstring& path) {
    const std::filesystem::path target(path);
    const std::wstring tempName = L"." + target.filename().wstring() + L".cpptmp";
    return (target.parent_path() / tempName).wstring();
}

}  // namespace

std::wstring DescribeWin32Error(unsigned long code) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message;
    if (length > 0 && buffer != nullptr) {
        message.assign(buffer, length);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    if (message.empty()) {
        message = L"未知错误(" + std::to_wstring(code) + L")";
    }
    return message;
}

WriteResult WriteFileAtomic(const std::wstring& path, std::string_view bytes) {
    WriteResult result;

    // 1. 只读属性处理
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        if (!::SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY)) {
            result.lastError = ::GetLastError();
            result.diskReadOnly = true;
            result.message = L"文件带有只读属性且无法清除：" + DescribeWin32Error(result.lastError);
            return result;
        }
    }

    // 2~3. 在目标目录创建临时文件并写入全部字节
    const std::wstring tempPath = TempPathFor(path);
    const HANDLE file = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.lastError = ::GetLastError();
        result.accessDenied = (result.lastError == ERROR_ACCESS_DENIED);
        result.diskFull = (result.lastError == ERROR_DISK_FULL ||
                           result.lastError == ERROR_HANDLE_DISK_FULL);
        result.message = L"无法创建临时文件：" + DescribeWin32Error(result.lastError);
        return result;
    }

    bool writeOk = true;
    size_t written = 0;
    while (written < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - written, static_cast<size_t>(1) << 20));
        DWORD chunkWritten = 0;
        if (::WriteFile(file, bytes.data() + written, chunk, &chunkWritten, nullptr) == 0 ||
            chunkWritten == 0) {
            writeOk = false;
            break;
        }
        written += chunkWritten;
    }
    if (writeOk) {
        ::FlushFileBuffers(file);   // 保证数据落到磁盘再替换
    }
    ::CloseHandle(file);

    if (!writeOk) {
        result.lastError = ::GetLastError();
        result.diskFull = (result.lastError == ERROR_DISK_FULL ||
                           result.lastError == ERROR_HANDLE_DISK_FULL);
        result.message = L"写入临时文件失败：" + DescribeWin32Error(result.lastError);
        ::DeleteFileW(tempPath.c_str());
        return result;
    }

    // 5. 优先 ReplaceFile（保留原文件属性与 ACL），失败则退化为 MoveFileEx
    if (::ReplaceFileW(path.c_str(), tempPath.c_str(), nullptr,
                       REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) == 0) {
        if (::MoveFileExW(tempPath.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            result.lastError = ::GetLastError();
            result.accessDenied = (result.lastError == ERROR_ACCESS_DENIED ||
                                   result.lastError == ERROR_SHARING_VIOLATION);
            result.diskFull = (result.lastError == ERROR_DISK_FULL);
            result.message = L"替换目标文件失败（文件可能被其他程序占用）：" +
                             DescribeWin32Error(result.lastError);
            ::DeleteFileW(tempPath.c_str());
            return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace npcc::io
