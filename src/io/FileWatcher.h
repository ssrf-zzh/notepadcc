#pragma once

#include <functional>
#include <memory>
#include <string>

namespace npcc::io {

enum class FileChangeKind {
    Modified,
    Deleted,
    Renamed,
    Replaced,
};

// 文件变更监视（设计文档 4.5）。
//
// 实现方式是监听每个文件**所在目录**的变更通知，再按文件名过滤出我们关心的
// 那些——Windows 没有"只监视一个文件"的高效接口。每个目录一个后台线程，
// 用 ReadDirectoryChangesW 的异步（overlapped）形式，这样停下来的时候可以
// 可靠地从等待中退出，而不是靠关句柄去打断它。
class FileWatcher {
public:
    FileWatcher();
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // 回调在**监视线程**上执行，调用方需要自己 marshal 回 UI 线程。
    void SetCallback(std::function<void(const std::wstring&, FileChangeKind)> callback);

    void Watch(const std::wstring& path);
    void Unwatch(const std::wstring& path);
    void Clear();

    // 供测试与诊断：当前监视的文件数
    std::size_t WatchedCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const wchar_t* FileChangeKindName(FileChangeKind kind) noexcept;

}  // namespace npcc::io
