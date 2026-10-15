#include "io/FileWatcher.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "infra/Log.h"

namespace npcc::io {
namespace {

constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                FILE_NOTIFY_CHANGE_FILE_NAME;

std::wstring UpperCase(std::wstring text) {
    if (!text.empty()) {
        ::CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
    }
    return text;
}

FileChangeKind KindFromAction(DWORD action) {
    switch (action) {
        case FILE_ACTION_REMOVED:
            return FileChangeKind::Deleted;
        case FILE_ACTION_RENAMED_OLD_NAME:
            return FileChangeKind::Renamed;
        case FILE_ACTION_ADDED:
        case FILE_ACTION_RENAMED_NEW_NAME:
            return FileChangeKind::Replaced;
        case FILE_ACTION_MODIFIED:
        default:
            return FileChangeKind::Modified;
    }
}

struct WatchedFile {
    std::wstring fullPath;
    std::wstring upperName;   // 文件名大写，用于不区分大小写的比较
};

}  // namespace

const wchar_t* FileChangeKindName(FileChangeKind kind) noexcept {
    switch (kind) {
        case FileChangeKind::Deleted:  return L"已被删除";
        case FileChangeKind::Renamed:  return L"已被改名";
        case FileChangeKind::Replaced: return L"已被替换";
        case FileChangeKind::Modified:
        default:                       return L"已被修改";
    }
}

struct DirectoryWatch {
    std::wstring             directory;
    HANDLE                   handle = INVALID_HANDLE_VALUE;
    HANDLE                   stopEvent = nullptr;
    HANDLE                   changeEvent = nullptr;
    // 第一次 ReadDirectoryChangesW 挂上去之后置位。Watch() 要等它，
    // 否则"开始监视"到"真正盯着"之间有个空档，这期间的改动会被漏掉。
    HANDLE                   armedEvent = nullptr;
    std::thread              thread;
    std::vector<WatchedFile> files;
};

// 与监视线程共享的状态，必须定义在 DirectoryWatch 之后。
// 放匿名命名空间是为了让自由函数能命名它——FileWatcher::Impl 是私有嵌套类型，
// 命名空间作用域的自由函数指不到。
struct WatchState {
    std::mutex                                   mutex;
    std::vector<std::unique_ptr<DirectoryWatch>> watches;
    std::function<void(const std::wstring&, FileChangeKind)> callback;
};

struct FileWatcher::Impl {
    WatchState state;
};

namespace {

void StopWatch(DirectoryWatch* watch) {
    if (watch->stopEvent != nullptr) {
        ::SetEvent(watch->stopEvent);
    }
    if (watch->thread.joinable()) {
        watch->thread.join();
    }
    if (watch->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(watch->handle);
        watch->handle = INVALID_HANDLE_VALUE;
    }
    if (watch->stopEvent != nullptr) {
        ::CloseHandle(watch->stopEvent);
        watch->stopEvent = nullptr;
    }
    if (watch->changeEvent != nullptr) {
        ::CloseHandle(watch->changeEvent);
        watch->changeEvent = nullptr;
    }
    if (watch->armedEvent != nullptr) {
        ::CloseHandle(watch->armedEvent);
        watch->armedEvent = nullptr;
    }
}

void WatchLoop(DirectoryWatch* watch, WatchState* state) {
    std::vector<char> buffer(64 * 1024);
    OVERLAPPED overlapped{};
    overlapped.hEvent = watch->changeEvent;
    const HANDLE waits[2] = {watch->changeEvent, watch->stopEvent};

    for (;;) {
        ::ResetEvent(watch->changeEvent);
        if (::ReadDirectoryChangesW(watch->handle, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), TRUE, kNotifyFilter,
                                    nullptr, &overlapped, nullptr) == 0) {
            return;
        }
        if (watch->armedEvent != nullptr) {
            ::SetEvent(watch->armedEvent);   // 已经开始盯着了
        }
        const DWORD waited = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (waited != WAIT_OBJECT_0) {
            // 收到停止信号：取消未完成的读再退出，而不是靠关句柄硬打断
            ::CancelIoEx(watch->handle, &overlapped);
            return;
        }
        DWORD bytes = 0;
        if (::GetOverlappedResult(watch->handle, &overlapped, &bytes, FALSE) == 0) {
            continue;
        }

        // 先取出要上报的项，再统一回调。
        //
        // 扫描必须在锁里做：目录通知里只有文件名，得对着"正在监视的文件"表筛一遍，
        // 而这张表会被 UI 线程的 Watch/Unwatch 改动。不加锁的话 push_back 触发的
        // 重分配会让这里读到已释放的内存，文件名匹配不上，这次改动就被静默丢掉了——
        // 用户看到的现象是"外面改了文件，编辑器毫无反应"。
        //
        // 回调则一定要在锁外发：调用方可能在回调里 Unwatch，持锁就会自锁死。
        std::vector<std::pair<std::wstring, FileChangeKind>> events;
        std::function<void(const std::wstring&, FileChangeKind)> callback;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            callback = state->callback;

            if (bytes == 0) {
                // 缓冲区溢出时 Windows 会把这一批通知整批丢掉，返回 0 字节。
                // 这里不能装作没事：按"这些文件都可能变了"上报，上层会重新比对
                // 文件的大小与时间戳，没真变就只是静默刷新记录，不会误报。
                for (const WatchedFile& file : watch->files) {
                    events.emplace_back(file.fullPath, FileChangeKind::Modified);
                }
            } else {
                auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
                for (;;) {
                    const std::wstring name(info->FileName,
                                           info->FileNameLength / sizeof(wchar_t));
                    const std::wstring upperName = UpperCase(name);
                    for (const WatchedFile& file : watch->files) {
                        if (file.upperName == upperName) {
                            events.emplace_back(file.fullPath, KindFromAction(info->Action));
                            break;
                        }
                    }
                    if (info->NextEntryOffset == 0) {
                        break;
                    }
                    info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                        reinterpret_cast<char*>(info) + info->NextEntryOffset);
                }
            }
        }
        if (!callback || events.empty()) {
            continue;
        }
        for (const auto& event : events) {
            callback(event.first, event.second);
        }
    }
}

}  // namespace

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() {
    Clear();
}

void FileWatcher::SetCallback(
    std::function<void(const std::wstring&, FileChangeKind)> callback) {
    std::lock_guard<std::mutex> guard(impl_->state.mutex);
    impl_->state.callback = std::move(callback);
}

std::size_t FileWatcher::WatchedCount() const {
    std::lock_guard<std::mutex> guard(impl_->state.mutex);
    std::size_t count = 0;
    for (const auto& watch : impl_->state.watches) {
        count += watch->files.size();
    }
    return count;
}

void FileWatcher::Watch(const std::wstring& path) {
    const std::filesystem::path full(path);
    const std::wstring directory = full.parent_path().wstring();
    const std::wstring name = full.filename().wstring();
    if (directory.empty() || name.empty()) {
        return;
    }
    const std::wstring upperName = UpperCase(name);

    std::lock_guard<std::mutex> guard(impl_->state.mutex);
    for (auto& watch : impl_->state.watches) {
        if (watch->directory != directory) {
            continue;
        }
        for (const WatchedFile& file : watch->files) {
            if (file.upperName == upperName) {
                return;   // 已在监视
            }
        }
        watch->files.push_back(WatchedFile{full.wstring(), upperName});
        return;
    }

    auto watch = std::make_unique<DirectoryWatch>();
    watch->directory = directory;
    watch->files.push_back(WatchedFile{full.wstring(), upperName});
    watch->handle = ::CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (watch->handle == INVALID_HANDLE_VALUE) {
        // 这里以前是静默 return：结果是"整个外部改动检测失效"却一点痕迹都没有
        infra::LogWarn(L"watch", L"无法监视目录（错误码 " +
                                    std::to_wstring(::GetLastError()) + L"）: " + directory);
        return;
    }
    watch->stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    watch->changeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    watch->armedEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (watch->stopEvent == nullptr || watch->changeEvent == nullptr ||
        watch->armedEvent == nullptr) {
        infra::LogWarn(L"watch", L"创建监视事件失败: " + directory);
        StopWatch(watch.get());
        return;
    }
    DirectoryWatch* raw = watch.get();
    WatchState* state = &impl_->state;
    watch->thread = std::thread([raw, state] { WatchLoop(raw, state); });
    // 等第一次监视真正挂上去再返回，堵住这段空档
    if (::WaitForSingleObject(watch->armedEvent, 3000) != WAIT_OBJECT_0) {
        // 挂载超时意味着"从现在起可能漏掉改动"，必须留下痕迹
        infra::LogWarn(L"watch", L"监视挂载超时，期间的文件改动可能被漏掉: " + directory);
    }
    infra::LogInfo(L"watch", L"已开始监视目录: " + directory);
    impl_->state.watches.push_back(std::move(watch));
}

void FileWatcher::Unwatch(const std::wstring& path) {
    const std::filesystem::path full(path);
    const std::wstring directory = full.parent_path().wstring();
    const std::wstring upperName = UpperCase(full.filename().wstring());

    std::unique_ptr<DirectoryWatch> doomed;
    {
        std::lock_guard<std::mutex> guard(impl_->state.mutex);
        for (auto it = impl_->state.watches.begin(); it != impl_->state.watches.end(); ++it) {
            if ((*it)->directory != directory) {
                continue;
            }
            auto& files = (*it)->files;
            files.erase(std::remove_if(files.begin(), files.end(),
                                       [&upperName](const WatchedFile& file) {
                                           return file.upperName == upperName;
                                       }),
                        files.end());
            if (files.empty()) {
                doomed = std::move(*it);
                impl_->state.watches.erase(it);
            }
            break;
        }
    }
    // 关键：join 必须在锁外做。持锁 join 时监视线程可能正等着同一把锁，
    // 直接死锁。
    if (doomed) {
        StopWatch(doomed.get());
    }
}

void FileWatcher::Clear() {
    std::vector<std::unique_ptr<DirectoryWatch>> taken;
    {
        std::lock_guard<std::mutex> guard(impl_->state.mutex);
        taken.swap(impl_->state.watches);
    }
    for (auto& watch : taken) {
        StopWatch(watch.get());
    }
}

}  // namespace npcc::io
