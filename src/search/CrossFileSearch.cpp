#include "search/CrossFileSearch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "search/ByteMatcher.h"
#include "search/FileScanner.h"
#include "search/TextBytes.h"

namespace npcc::search {
namespace {

constexpr std::size_t kBatchSize = 64;
constexpr auto        kProgressInterval = std::chrono::milliseconds(120);

FileSearchResult ScanOneFile(const std::wstring& path, const SearchConfig& config) {
    FileSearchResult result;
    result.path = path;

    PreparedFile file;
    if (!PrepareFile(path, config, file)) {
        if (file.binary) {
            result.binary = true;
        } else {
            result.error = file.error;
        }
        return result;
    }
    result.fileSize = file.fileSize;

    // 每个工作线程缓存一个匹配器：绝大多数文件编码相同，
    // 这样正则只需编译一次，而不是每个文件都编译。
    thread_local ByteMatcher matcher;
    if (!matcher.Matches(config, file.utfMode, file.codePage)) {
        SearchOutcome prepareOutcome;
        if (!matcher.Prepare(config, file.utfMode, file.codePage, prepareOutcome)) {
            result.error = prepareOutcome.message;
            return result;
        }
    }

    std::vector<MatchRange> matches;
    bool truncated = false;
    SearchOutcome outcome;
    if (!matcher.FindAll(file.bytes, matches, config.maxHitsPerFile, truncated, outcome)) {
        result.error = outcome.message;
        return result;
    }
    result.truncated = truncated;
    if (matches.empty()) {
        return result;
    }

    const std::string& text = file.bytes;
    const std::vector<std::size_t> lineStarts = BuildLineStarts(text);
    for (const MatchRange& match : matches) {
        const std::size_t start = static_cast<std::size_t>(match.start);
        const std::size_t lineIndex = LineIndexFor(lineStarts, start);
        const std::size_t lineBegin = lineStarts[lineIndex];
        const std::int64_t column = static_cast<std::int64_t>(start) -
                                    static_cast<std::int64_t>(lineBegin);
        MatchRange local{column, column + (match.end - match.start)};

        if (!result.hits.empty() &&
            result.hits.back().line == static_cast<std::int32_t>(lineIndex + 1)) {
            result.hits.back().matches.push_back(local);
            continue;
        }
        LineHit hit;
        hit.line = static_cast<std::int32_t>(lineIndex + 1);
        // 行文本统一转成 UTF-8：界面层就不必再关心每个文件原本的编码
        hit.text = DocBytesToUtf8(LineTextAt(text, lineStarts, lineIndex), file.utfMode,
                                  file.codePage);
        hit.matches.push_back(local);
        result.hits.push_back(std::move(hit));
    }
    return result;
}

}  // namespace

CrossFileSearch::CrossFileSearch(unsigned workerThreads) : pool_(workerThreads) {}
CrossFileSearch::~CrossFileSearch() = default;

std::shared_ptr<CrossSearchJob> CrossFileSearch::Start(const SearchConfig& config,
                                                       CrossSearchCallbacks callbacks) {
    auto job = std::make_shared<CrossSearchJob>();
    // 捕获 shared_ptr 让任务在线程运行期间保持存活
    job->coordinator_ = std::thread([this, job, config, callbacks]() {
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

            std::mutex                    resultsMutex;
            std::vector<FileSearchResult> results;
            std::mutex                    doneMutex;
            std::condition_variable       doneSignal;
            auto                          remaining =
                std::make_shared<std::atomic<std::size_t>>(end - offset);

            for (std::size_t index = offset; index < end; ++index) {
                const std::wstring path = files[index];
                pool_.Submit([&, path, remaining]() {
                    if (!job->IsCancelled()) {
                        FileSearchResult scanned = ScanOneFile(path, config);
                        std::lock_guard<std::mutex> guard(resultsMutex);
                        results.push_back(std::move(scanned));
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

            // 结果统一在协调线程上回调，保证调用方只面对一个线程
            for (const FileSearchResult& result : results) {
                ++progress.filesScanned;
                progress.currentFile = result.path;
                if (result.binary) {
                    ++progress.skippedBinary;
                } else if (!result.error.empty()) {
                    ++progress.failedFiles;
                } else if (!result.hits.empty()) {
                    ++progress.filesMatched;
                    for (const LineHit& hit : result.hits) {
                        progress.hits += hit.matches.size();
                    }
                    if (callbacks.onFileResult) {
                        callbacks.onFileResult(result);
                    }
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

std::vector<FileSearchResult> CrossFileSearch::RunSynchronously(const SearchConfig& config,
                                                                unsigned workerThreads) {
    CrossFileSearch search(workerThreads);
    std::vector<FileSearchResult> results;
    std::mutex mutex;

    CrossSearchCallbacks callbacks;
    callbacks.onFileResult = [&mutex, &results](const FileSearchResult& result) {
        std::lock_guard<std::mutex> guard(mutex);
        results.push_back(result);
    };

    auto job = search.Start(config, std::move(callbacks));
    job->Wait();
    std::sort(results.begin(), results.end(),
              [](const FileSearchResult& a, const FileSearchResult& b) { return a.path < b.path; });
    return results;
}

}  // namespace npcc::search
