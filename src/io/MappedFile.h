#pragma once

#include <cstdint>
#include <string>

namespace npcc::io {

// 只读内存映射文件。x64 下不受 2 GB 限制，映射本身不复制内容——
// 这正是设计文档 6.1 里大文件方案能成立的前提。
class MappedFile {
public:
    MappedFile() = default;
    explicit MappedFile(const std::wstring& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    bool Open(const std::wstring& path);
    void Close();

    bool          Valid() const noexcept { return data_ != nullptr; }
    const char*   Data() const noexcept { return data_; }
    std::uint64_t Size() const noexcept { return size_; }
    const std::wstring& Error() const noexcept { return error_; }

private:
    void*         handle_ = nullptr;   // HANDLE
    void*         mapping_ = nullptr;  // HANDLE
    const char*   data_ = nullptr;
    std::uint64_t size_ = 0;
    std::wstring  error_;
};

}  // namespace npcc::io
