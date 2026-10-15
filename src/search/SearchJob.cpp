#include "search/SearchJob.h"

namespace npcc::search {

CrossSearchJob::~CrossSearchJob() {
    if (!coordinator_.joinable()) {
        return;
    }
    // 任务对象可能正是被协调线程自己释放的（它是线程函数的捕获之一）。
    // 那种情况下不能 join 自己，thread 析构前 detach 即可——线程马上就结束了。
    if (coordinator_.get_id() == std::this_thread::get_id()) {
        coordinator_.detach();
    } else {
        coordinator_.join();
    }
}

SearchProgress CrossSearchJob::Progress() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return progress_;
}

void CrossSearchJob::SetProgress(const SearchProgress& progress) {
    std::lock_guard<std::mutex> guard(mutex_);
    progress_ = progress;
}

void CrossSearchJob::MarkFinished(const SearchProgress& progress) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        progress_ = progress;
        progress_.finished = true;
        finished_ = true;
    }
    finishedSignal_.notify_all();
}

void CrossSearchJob::Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    finishedSignal_.wait(lock, [this] { return finished_; });
    lock.unlock();
    if (coordinator_.joinable()) {
        coordinator_.join();
    }
}

}  // namespace npcc::search
