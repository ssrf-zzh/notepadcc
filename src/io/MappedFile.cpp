#include "io/MappedFile.h"

#include <windows.h>

#include "io/AtomicFile.h"

namespace npcc::io {

MappedFile::MappedFile(const std::wstring& path) {
    Open(path);
}

MappedFile::~MappedFile() {
    Close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : handle_(other.handle_),
      mapping_(other.mapping_),
      data_(other.data_),
      size_(other.size_),
      error_(std::move(other.error_)) {
    other.handle_ = nullptr;
    other.mapping_ = nullptr;
    other.data_ = nullptr;
    other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        mapping_ = other.mapping_;
        data_ = other.data_;
        size_ = other.size_;
        error_ = std::move(other.error_);
        other.handle_ = nullptr;
        other.mapping_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void MappedFile::Close() {
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    size_ = 0;
}

bool MappedFile::Open(const std::wstring& path) {
    Close();
    error_.clear();

    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error_ = L"无法打开文件：" + DescribeWin32Error(::GetLastError());
        return false;
    }
    handle_ = file;

    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == 0) {
        error_ = L"读取文件大小失败：" + DescribeWin32Error(::GetLastError());
        Close();
        return false;
    }
    size_ = static_cast<std::uint64_t>(size.QuadPart);
    if (size_ == 0) {
        // 空文件不建立映射：长度为 0 的文件映射视图没有意义
        return true;
    }

    const HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        error_ = L"建立文件映射失败：" + DescribeWin32Error(::GetLastError());
        Close();
        return false;
    }
    mapping_ = mapping;

    const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        error_ = L"映射文件视图失败：" + DescribeWin32Error(::GetLastError());
        Close();
        return false;
    }
    data_ = static_cast<const char*>(view);
    return true;
}

}  // namespace npcc::io
