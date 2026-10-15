#include "search/CrossFileReplace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

#include "edit/Encoding.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"
#include "search/ByteMatcher.h"
#include "search/FileScanner.h"
#include "search/TextBytes.h"

namespace npcc::search {
namespace {

constexpr std::size_t kBatchSize = 32;
constexpr auto        kProgressInterval = std::chrono::milliseconds(120);

// 把编辑器域字节里的匹配全部替换掉，得到新的编辑器域字节。
// 匹配本来就是有序且不重叠的，所以正向拼装即可，不需要倒着改。
bool BuildReplacedContent(std::string_view bytes, const std::vector<MatchRange>& matches,
                          const std::vector<std::string>& replacements, std::string& out) {
    out.clear();
    out.reserve(bytes.size() + 64);
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < matches.size(); ++i) {
        const std::size_t start = static_cast<std::size_t>(matches[i].start);
        const std::size_t end = static_cast<std::size_t>(matches[i].end);
        if (start < cursor || end > bytes.size()) {
            return false;   // 区间非法或重叠，宁可不动
        }
        out.append(bytes.substr(cursor, start - cursor));
        out.append(replacements[i]);
        cursor = end;
    }
    out.append(bytes.substr(cursor));
    return true;
}

// 计算一个文件替换后的内容。失败原因写进 outcome。
bool ComputeReplacement(const SearchConfig& config, const PreparedFile& file,
                        ByteMatcher& matcher, std::string& newBytes, std::size_t& count,
                        bool& truncated, SearchOutcome& outcome) {
    std::vector<MatchRange> matches;
    if (!matcher.FindAll(file.bytes, matches, config.maxHitsPerFile, truncated, outcome)) {
        return false;
    }
    count = matches.size();
    if (matches.empty()) {
        newBytes.clear();
        return true;
    }

    // ANSI 文件要额外确认替换串在本代码页里能无损表示，
    // 否则 WideCharToMultiByte 会把无法表示的字符静默换成 '?'。
    if (!file.utfMode && !CanRepresentInCodePage(config.replaceTo, file.codePage)) {
        outcome = SearchOutcome::Failure(
            SearchError::EncodingFailed,
            L"替换内容含该文件编码无法表示的字符，已跳过这个文件。");
        return false;
    }

    // 正则的 $1 展开依赖"最近一次匹配"，必须先把全部替换串算出来再动内容
    std::vector<std::string> replacements;
    replacements.reserve(matches.size());
    for (const MatchRange& match : matches) {
        std::string text;
        if (!matcher.ExpandReplacement(file.bytes, match.start, config.replaceTo, text, outcome)) {
            return false;
        }
        replacements.push_back(std::move(text));
    }
    return BuildReplacedContent(file.bytes, matches, replacements, newBytes);
}

ReplaceSample MakeSample(std::string_view before, std::string_view after, std::int32_t line,
                         std::size_t maxChars) {
    ReplaceSample sample;
    sample.line = line;
    sample.before.assign(before.substr(0, std::min(before.size(), maxChars)));
    sample.after.assign(after.substr(0, std::min(after.size(), maxChars)));
    if (before.size() > maxChars) {
        sample.before += "...";
    }
    if (after.size() > maxChars) {
        sample.after += "...";
    }
    return sample;
}

// 生成预览样本：逐行对比替换前后，只取前若干处有差异的行
void CollectSamples(std::string_view beforeBytes, std::string_view afterBytes,
                    const ReplaceOptions& options, std::vector<ReplaceSample>& out) {
    const std::vector<std::size_t> beforeStarts = BuildLineStarts(beforeBytes);
    const std::vector<std::size_t> afterStarts = BuildLineStarts(afterBytes);
    const std::size_t lineCount = std::min(beforeStarts.size(), afterStarts.size());

    for (std::size_t line = 0; line < lineCount && out.size() < options.samplesPerFile; ++line) {
        const std::string_view before = LineTextAt(beforeBytes, beforeStarts, line);
        const std::string_view after = LineTextAt(afterBytes, afterStarts, line);
        if (before != after) {
            out.push_back(MakeSample(before, after, static_cast<std::int32_t>(line + 1),
                                     options.maxSampleChars));
        }
    }
}

FileReplacePlan PlanOneFile(const std::wstring& path, const SearchConfig& config,
                            const ReplaceOptions& options) {
    FileReplacePlan plan;
    plan.path = path;

    PreparedFile file;
    if (!PrepareFile(path, config, file)) {
        if (!file.binary) {
            plan.error = file.error;
        }
        return plan;
    }
    plan.fileSize = file.fileSize;
    plan.lastWriteMs = file.lastWriteMs;

    thread_local ByteMatcher matcher;
    if (!matcher.Matches(config, file.utfMode, file.codePage)) {
        SearchOutcome prepareOutcome;
        if (!matcher.Prepare(config, file.utfMode, file.codePage, prepareOutcome)) {
            plan.error = prepareOutcome.message;
            return plan;
        }
    }

    std::string newBytes;
    std::size_t count = 0;
    bool truncated = false;
    SearchOutcome outcome;
    if (!ComputeReplacement(config, file, matcher, newBytes, count, truncated, outcome)) {
        plan.error = outcome.message;
        return plan;
    }
    plan.replacements = count;
    plan.truncated = truncated;
    if (count > 0) {
        CollectSamples(file.bytes, newBytes, options, plan.samples);
    }
    return plan;
}

// 写回一个文件：重新读取 → 校验身份 → 重算内容 → 原子写回
bool ApplyOneFile(const SearchConfig& config, const ReplaceOptions& options,
                  const FileReplacePlan& plan, std::size_t& replacements, std::wstring& error) {
    std::uint64_t size = 0;
    std::uint64_t lastWrite = 0;
    if (!QueryFileIdentity(plan.path, size, lastWrite)) {
        error = L"文件已不存在或无法访问。";
        return false;
    }
    if (size != plan.fileSize || lastWrite != plan.lastWriteMs) {
        // 生成预览之后文件被外部改动过，不能照着旧内容覆盖
        error = L"文件在生成预览之后已被修改，已跳过以免覆盖他人的改动。";
        return false;
    }

    PreparedFile file;
    if (!PrepareFile(plan.path, config, file)) {
        error = file.error.empty() ? std::wstring(L"文件无法读取。") : file.error;
        return false;
    }

    thread_local ByteMatcher matcher;
    if (!matcher.Matches(config, file.utfMode, file.codePage)) {
        SearchOutcome prepareOutcome;
        if (!matcher.Prepare(config, file.utfMode, file.codePage, prepareOutcome)) {
            error = prepareOutcome.message;
            return false;
        }
    }

    std::string newBytes;
    std::size_t count = 0;
    bool truncated = false;
    SearchOutcome outcome;
    if (!ComputeReplacement(config, file, matcher, newBytes, count, truncated, outcome)) {
        error = outcome.message;
        return false;
    }
    if (count == 0) {
        error = L"重算后没有匹配项，未做改动。";
        return false;
    }

    // 编码回写：从原始字节重新判定一次，保证 BOM 与 UTF-16 字节序不被改掉
    edit::EncodingState state;
    state.uniMode = file.utfMode ? edit::UniMode::Utf8 : edit::UniMode::Ansi;
    state.codePage = file.codePage;
    if (const io::ReadResult raw = io::ReadWholeFile(plan.path, config.maxFileSizeBytes); raw.ok) {
        const edit::LoadResult loaded = edit::DetectAndPrepare(raw.bytes, file.codePage);
        state = loaded.encoding;
        state.codePage = file.codePage;
    }
    const std::string outBytes = edit::EncodeForSave(newBytes, state);
    if (outBytes.empty() && !newBytes.empty()) {
        error = L"按原编码回写失败，未改动文件。";
        return false;
    }

    if (options.keepBackup) {
        std::error_code ec;
        std::filesystem::path backup(plan.path);
        backup += L".bak";
        std::filesystem::copy_file(plan.path, backup,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

    const io::WriteResult written = io::WriteFileAtomic(plan.path, outBytes);
    if (!written.ok) {
        error = written.message;
        return false;
    }
    replacements = count;
    return true;
}

}  // namespace

CrossFileReplace::CrossFileReplace(unsigned workerThreads) : pool_(workerThreads) {}
CrossFileReplace::~CrossFileReplace() = default;

std::shared_ptr<CrossSearchJob> CrossFileReplace::BuildPlan(const SearchConfig& config,
                                                           const ReplaceOptions& options,
                                                           ReplacePlanCallbacks callbacks) {
    auto job = std::make_shared<CrossSearchJob>();
    job->coordinator_ = std::thread([this, job, config, options, callbacks]() {
        SearchProgress progress;

        std::vector<std::wstring> files;
        EnumerateFiles(config, files, [&job] { return job->IsCancelled(); });
        progress.filesTotal = files.size();
        job->SetProgress(progress);
        if (callbacks.onProgress) {
            callbacks.onProgress(progress);
        }

        auto lastReport = std::chrono::steady_clock::now();
        for (std::size_t offset = 0; offset < files.size();) {
            if (job->IsCancelled()) {
                break;
            }
            const std::size_t end = std::min(files.size(), offset + kBatchSize);

            std::mutex                   plansMutex;
            std::vector<FileReplacePlan> plans;
            std::mutex                   doneMutex;
            std::condition_variable      doneSignal;
            auto                         remaining =
                std::make_shared<std::atomic<std::size_t>>(end - offset);

            for (std::size_t index = offset; index < end; ++index) {
                const std::wstring path = files[index];
                pool_.Submit([&, path, remaining]() {
                    if (!job->IsCancelled()) {
                        FileReplacePlan plan = PlanOneFile(path, config, options);
                        std::lock_guard<std::mutex> guard(plansMutex);
                        plans.push_back(std::move(plan));
                    }
                    if (remaining->fetch_sub(1) == 1) {
                        std::lock_guard<std::mutex> guard(doneMutex);
                        doneSignal.notify_all();
                    }
                });
            }
            {
                std::unique_lock<std::mutex> lock(doneMutex);
                doneSignal.wait(lock, [&remaining] { return remaining->load() == 0; });
            }
            offset = end;

            for (const FileReplacePlan& plan : plans) {
                ++progress.filesScanned;
                progress.currentFile = plan.path;
                if (!plan.error.empty()) {
                    ++progress.failedFiles;
                    // 出错的文件也上报：界面要能告诉用户"这几个文件为什么没改"
                    if (callbacks.onPlan) {
                        callbacks.onPlan(plan);
                    }
                    continue;
                }
                if (plan.replacements == 0) {
                    continue;   // 没有匹配的文件不进预览
                }
                ++progress.filesMatched;
                progress.hits += plan.replacements;
                if (callbacks.onPlan) {
                    callbacks.onPlan(plan);
                }
            }
            job->SetProgress(progress);

            const auto now = std::chrono::steady_clock::now();
            if (callbacks.onProgress && (now - lastReport) >= kProgressInterval) {
                lastReport = now;
                callbacks.onProgress(progress);
            }
        }

        progress.cancelled = job->IsCancelled();
        progress.finished = true;
        job->MarkFinished(progress);
        if (callbacks.onFinished) {
            callbacks.onFinished(progress);
        }
    });
    return job;
}

std::vector<FileReplacePlan> CrossFileReplace::BuildPlanSync(const SearchConfig& config,
                                                             const ReplaceOptions& options,
                                                             unsigned workerThreads) {
    CrossFileReplace replace(workerThreads);
    std::vector<FileReplacePlan> plans;
    std::mutex mutex;

    ReplacePlanCallbacks callbacks;
    callbacks.onPlan = [&mutex, &plans](const FileReplacePlan& plan) {
        std::lock_guard<std::mutex> guard(mutex);
        plans.push_back(plan);
    };

    auto job = replace.BuildPlan(config, options, std::move(callbacks));
    job->Wait();
    std::sort(plans.begin(), plans.end(),
              [](const FileReplacePlan& a, const FileReplacePlan& b) { return a.path < b.path; });
    return plans;
}

ReplaceReport CrossFileReplace::Apply(const SearchConfig& config, const ReplaceOptions& options,
                                      const std::vector<FileReplacePlan>& plans) {
    ReplaceReport report;
    for (const FileReplacePlan& plan : plans) {
        if (!plan.selected) {
            ++report.skipped;
            continue;
        }
        if (!plan.error.empty()) {
            ++report.failed;
            report.errors.push_back(plan.path + L"：" + plan.error);
            continue;
        }
        std::size_t replacements = 0;
        std::wstring error;
        if (ApplyOneFile(config, options, plan, replacements, error)) {
            ++report.applied;
            report.replacements += replacements;
        } else {
            // 文件被外部改动属于"跳过"而不是"失败"：原文件是完好无损的
            if (error.find(L"已被修改") != std::wstring::npos ||
                error.find(L"没有匹配项") != std::wstring::npos) {
                ++report.skipped;
            } else {
                ++report.failed;
            }
            report.errors.push_back(plan.path + L"：" + error);
        }
    }
    return report;
}

}  // namespace npcc::search
