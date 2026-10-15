#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "infra/ThreadPool.h"
#include "search/SearchConfig.h"
#include "search/SearchJob.h"
#include "search/SearchResult.h"

namespace npcc::search {

struct CrossSearchCallbacks {
    // 三个回调都在**协调线程**上调用，不是 UI 线程。
    // 调用方若要更新界面，必须自己 PostMessage 回 UI 线程（设计文档 1.5）。
    std::function<void(const FileSearchResult&)> onFileResult;
    std::function<void(const SearchProgress&)>   onProgress;   // 已按时间节流
    std::function<void(const SearchProgress&)>   onFinished;
};

// 在文件中查找（设计文档 2.5 第 4 条、5.5）。
// 目录遍历在协调线程上做，文件内容扫描分发到线程池。
// 本类可同时承载多个任务：配置与回调随任务传递，不存放在成员里。
//
// 生命周期约束：Start() 返回的任务会在线程池上派活，因此**本对象必须比所有
// 已启动的任务活得久**。MainWindow 在自身生命周期内持有它，满足该约束。
class CrossFileSearch {
public:
    explicit CrossFileSearch(unsigned workerThreads = 0);
    ~CrossFileSearch();

    CrossFileSearch(const CrossFileSearch&) = delete;
    CrossFileSearch& operator=(const CrossFileSearch&) = delete;

    std::shared_ptr<CrossSearchJob> Start(const SearchConfig& config,
                                          CrossSearchCallbacks callbacks);

    // 供测试与调试：同步跑完一次搜索，返回所有结果
    static std::vector<FileSearchResult> RunSynchronously(const SearchConfig& config,
                                                          unsigned workerThreads = 1);

private:
    infra::ThreadPool pool_;
};

}  // namespace npcc::search
