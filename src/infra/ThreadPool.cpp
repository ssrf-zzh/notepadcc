#include "infra/ThreadPool.h"

#include <algorithm>

namespace npcc::infra {
namespace {

unsigned DefaultThreadCount() {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 2) {
        return 1;
    }
    return std::min(4u, hardware - 1);
}

}  // namespace

ThreadPool::ThreadPool(unsigned threadCount) {
    const unsigned count = threadCount == 0 ? DefaultThreadCount() : threadCount;
    workers_.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::Submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        jobs_.push(std::move(job));
    }
    wake_.notify_one();
}

void ThreadPool::WaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && running_ == 0; });
}

void ThreadPool::WorkerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop();
            ++running_;
        }

        job();

        {
            std::lock_guard<std::mutex> guard(mutex_);
            --running_;
            if (jobs_.empty() && running_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

}  // namespace npcc::infra
