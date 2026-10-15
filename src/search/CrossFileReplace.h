#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "infra/ThreadPool.h"
#include "search/SearchConfig.h"
#include "search/SearchJob.h"
#include "search/SearchResult.h"

namespace npcc::search {

// 一处替换的前后对照（预览用），文本都是 UTF-8
struct ReplaceSample {
    std::int32_t line = 0;
    std::string  before;
    std::string  after;
};

// 一个文件的替换计划。
//
// 计划里**不保存文件内容**：应用时会重新读取并校验文件身份。这样即使
// "生成预览"和"写回"之间隔了用户思考的时间，也不会照着过期的内容覆盖文件，
// 一旦发现文件被外部改动就跳过它。
struct FileReplacePlan {
    std::wstring                path;
    std::uint64_t               fileSize = 0;
    std::uint64_t               lastWriteMs = 0;
    std::size_t                 replacements = 0;
    std::vector<ReplaceSample>  samples;
    bool                        selected = true;   // 用户可在预览里取消勾选
    std::wstring                error;            // 非空表示该文件不参与替换
    bool                        truncated = false;
};

struct ReplaceReport {
    std::size_t applied = 0;        // 成功写回的文件数
    std::size_t failed = 0;         // 写回失败或内容无法表示
    std::size_t skipped = 0;        // 未勾选，或生成计划后文件被外部改动
    std::size_t replacements = 0;   // 实际替换处数
    bool        cancelled = false;
    std::vector<std::wstring> errors;
};

struct ReplaceOptions {
    bool        keepBackup = false;      // 写回前生成 .bak
    std::size_t samplesPerFile = 3;      // 每个文件保留几处前后对照
    std::size_t maxSampleChars = 200;    // 单条预览文本的长度上限
};

struct ReplacePlanCallbacks {
    // 与搜索一样，三个回调都在**协调线程**上调用，调用方需自行 marshal 回 UI 线程
    std::function<void(const FileReplacePlan&)> onPlan;
    std::function<void(const SearchProgress&)>  onProgress;
    std::function<void(const SearchProgress&)>  onFinished;
};

// 在文件中替换（设计文档 2.5、5.5）。
//
// 分两步：先 BuildPlan 生成计划（只读盘），用户确认后再 Apply 写回。
// 不做"边搜边改"，因为这是会批量改动用户文件的操作，必须留出确认环节。
class CrossFileReplace {
public:
    explicit CrossFileReplace(unsigned workerThreads = 0);
    ~CrossFileReplace();

    CrossFileReplace(const CrossFileReplace&) = delete;
    CrossFileReplace& operator=(const CrossFileReplace&) = delete;

    // 生成替换计划。本对象必须比返回的任务活得久。
    std::shared_ptr<CrossSearchJob> BuildPlan(const SearchConfig& config,
                                              const ReplaceOptions& options,
                                              ReplacePlanCallbacks callbacks);

    // 同步版本，供测试使用
    static std::vector<FileReplacePlan> BuildPlanSync(const SearchConfig& config,
                                                      const ReplaceOptions& options,
                                                      unsigned workerThreads = 1);

    // 应用计划：逐个文件重新读取 → 校验身份 → 原子写回。
    // 单个文件失败不影响其余文件，结果汇总在返回值里。
    static ReplaceReport Apply(const SearchConfig& config, const ReplaceOptions& options,
                               const std::vector<FileReplacePlan>& plans);

private:
    infra::ThreadPool pool_;
};

}  // namespace npcc::search
