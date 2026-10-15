#include "io/FileReader.h"

#include <windows.h>

#include "io/AtomicFile.h"

namespace npcc::io {

ReadResult ReadWholeFile(const std::wstring& path, uint64_t maxBytes) {
    ReadResult result;

    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.lastError = ::GetLastError();
        result.message = L"无法打开文件：" + DescribeWin32Error(result.lastError);
        return result;
    }

    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == 0) {
        result.lastError = ::GetLastError();
        result.message = L"读取文件大小失败：" + DescribeWin32Error(result.lastError);
        ::CloseHandle(file);
        return result;
    }
    result.size = static_cast<uint64_t>(size.QuadPart);

    if (result.size > maxBytes) {
        result.tooLarge = true;
        result.message = L"文件过大，M0 版本暂不支持打开（上限 64 MB）";
        ::CloseHandle(file);
        return result;
    }

    result.bytes.resize(static_cast<size_t>(result.size));
    size_t readTotal = 0;
    while (readTotal < result.bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (result.bytes.size() - readTotal) > (1u << 20) ? (1u << 20)
                                                           : (result.bytes.size() - readTotal));
        DWORD got = 0;
        if (::ReadFile(file, result.bytes.data() + readTotal, chunk, &got, nullptr) == 0) {
            result.lastError = ::GetLastError();
            result.message = L"读取文件失败：" + DescribeWin32Error(result.lastError);
            ::CloseHandle(file);
            return result;
        }
        if (got == 0) {
            break;   // 文件在读取过程中被截断
        }
        readTotal += got;
    }
    result.bytes.resize(readTotal);
    ::CloseHandle(file);

    result.ok = true;
    return result;
}

bool QueryFileIdentity(const std::wstring& path, uint64_t& size, uint64_t& lastWriteMs) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) {
        return false;
    }
    ULARGE_INTEGER fileSize{};
    fileSize.LowPart = data.nFileSizeLow;
    fileSize.HighPart = data.nFileSizeHigh;
    size = fileSize.QuadPart;
    ULARGE_INTEGER written{};
    written.LowPart = data.ftLastWriteTime.dwLowDateTime;
    written.HighPart = data.ftLastWriteTime.dwHighDateTime;
    lastWriteMs = written.QuadPart / 10000ull;   // 100ns -> ms
    return true;
}

}  // namespace npcc::io
