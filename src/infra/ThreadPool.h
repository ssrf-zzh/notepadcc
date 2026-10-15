#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace npcc::infra {

// 极简线程池（设计文档 1.5 的工作线程模型）。
// 提交的任务保证不会在调用线程上执行；析构时等待所有任务结束。
class ThreadPool {
public:
    // threadCount 为 0 时取 min(4, 硬件并发数 - 1)，且至少 1。
    explicit ThreadPool(unsigned threadCount = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void   Submit(std::function<void()> job);
    void   WaitIdle();
    unsigned ThreadCount() const noexcept { return static_cast<unsigned>(workers_.size()); }

private:
    void WorkerLoop();

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                        mutex_;
    std::condition_variable           wake_;
    std::condition_variable           idle_;
    std::size_t                       running_ = 0;
    bool                              stopping_ = false;
};

}  // namespace npcc::infra
