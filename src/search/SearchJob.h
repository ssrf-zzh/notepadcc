#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "search/SearchResult.h"

namespace npcc::search {

// 一次后台搜索/替换任务的句柄。Cancel() 可从任意线程调用。
// 生命周期：任务对象由调用方持有；协调线程也持有一份引用，
// 因此它不会被提前析构，析构时也不会 join 自己。
class CrossSearchJob {
public:
    ~CrossSearchJob();

    void Cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool IsCancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

    SearchProgress Progress() const;
    void           Wait();

private:
    friend class CrossFileSearch;
    friend class CrossFileReplace;

    void SetProgress(const SearchProgress& progress);
    void MarkFinished(const SearchProgress& progress);

    std::atomic<bool>       cancelled_{false};
    mutable std::mutex      mutex_;
    std::condition_variable finishedSignal_;
    SearchProgress          progress_;
    bool                    finished_ = false;
    std::thread             coordinator_;
};

}  // namespace npcc::search
