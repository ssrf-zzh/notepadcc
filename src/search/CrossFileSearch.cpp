#include "search/CrossFileSearch.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <windows.h>

#include "edit/Encoding.h"
#include "infra/Log.h"
#include "io/FileReader.h"
#include "search/ByteMatcher.h"
#include "search/TextBytes.h"
#include "search/Wildcard.h"

namespace npcc::search {
namespace {

constexpr std::uint32_t kFallbackCodePage = 936;
constexpr std::size_t   kBatchSize = 64;
constexpr auto          kProgressInterval = std::chrono::milliseconds(120);
constexpr std::size_t   kBinaryProbeBytes = 8192;

// 目录名黑名单：这些目录里几乎不可能有用户想找的文本，
// 遍历它们只会浪费时间，还会把版本库内部对象翻出来。
const wchar_t* const kDefaultExcludedDirs[] = {
    L".git", L".svn", L".hg", L"node_modules", L"__pycache__", L".vs", L".idea", L".gradle",
};

bool IsDefaultExcludedDir(std::wstring_view name) {
    const std::wstring owned(name);
    for (const wchar_t* candidate : kDefaultExcludedDirs) {
        if (::CompareStringOrdinal(owned.c_str(), -1, candidate, -1, TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

// 二进制判定：出现 NUL 字节，或控制字符占比过高（换行/制表符除外）
bool LooksBinary(std::string_view bytes) {
    const std::size_t probe = std::min(bytes.size(), kBinaryProbeBytes);
    if (probe == 0) {
        return false;
    }
    std::size_t suspicious = 0;
    for (std::size_t i = 0; i < probe; ++i) {
        const unsigned char ch = static_cast<unsigned char>(bytes[i]);
        if (ch == 0) {
            return true;
        }
        if (ch < 0x09 || (ch > 0x0D && ch < 0x20)) {
            ++suspicious;
        }
    }
    return suspicious * 100 > probe * 30;
}

std::vector<std::size_t> BuildLineStarts(std::string_view text) {
    std::vector<std::size_t> starts;
    starts.push_back(0);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

// 返回 0 基行号
std::size_t LineIndexFor(const std::vector<std::size_t>& lineStarts, std::size_t offset) {
    const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
    if (it == lineStarts.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(lineStarts.begin(), it) - 1);
}

std::string_view LineTextAt(std::string_view text, const std::vector<std::size_t>& lineStarts,
                            std::size_t lineIndex) {
    if (lineIndex >= lineStarts.size()) {
        return {};
    }
    const std::size_t begin = lineStarts[lineIndex];
    const std::size_t end =
        (lineIndex + 1 < lineStarts.size()) ? lineStarts[lineIndex + 1] : text.size();
    std::string_view line = text.substr(begin, end - begin);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

FileSearchResult ScanOneFile(const std::wstring& path, const SearchConfig& config) {
    FileSearchResult result;
    result.path = path;

    const io::ReadResult read = io::ReadWholeFile(path, config.maxFileSizeBytes);
    if (!read.ok) {
        result.error = read.message;
        return result;
    }
    result.fileSize = read.size;

    if (config.skipBinary && LooksBinary(read.bytes)) {
        result.binary = true;
        return result;
    }

    const edit::LoadResult loaded = edit::DetectAndPrepare(read.bytes, kFallbackCodePage);
    const bool utfMode = loaded.encoding.uniMode != edit::UniMode::Ansi;

    // 每个工作线程缓存一个匹配器：绝大多数文件编码相同，
    // 这样正则只需编译一次，而不是每个文件都编译。
    thread_local ByteMatcher matcher;
    if (!matcher.Matches(config, utfMode, loaded.encoding.codePage)) {
        SearchOutcome prepareOutcome;
        if (!matcher.Prepare(config, utfMode, loaded.encoding.codePage, prepareOutcome)) {
            result.error = prepareOutcome.message;
            return result;
        }
    }

    std::vector<MatchRange> matches;
    bool truncated = false;
    SearchOutcome outcome;
    if (!matcher.FindAll(loaded.editorBytes, matches, config.maxHitsPerFile, truncated, outcome)) {
        result.error = outcome.message;
        return result;
    }
    result.truncated = truncated;
    if (matches.empty()) {
        return result;
    }

    const std::string& text = loaded.editorBytes;
    const std::vector<std::size_t> lineStarts = BuildLineStarts(text);
    for (const MatchRange& match : matches) {
        const std::size_t start = static_cast<std::size_t>(match.start);
        const std::size_t lineIndex = LineIndexFor(lineStarts, start);
        const std::size_t lineBegin = lineStarts[lineIndex];
        const std::int64_t column = static_cast<std::int64_t>(start) -
                                    static_cast<std::int64_t>(lineBegin);
        MatchRange local{column, column + (match.end - match.start)};

        if (!result.hits.empty() && result.hits.back().line == static_cast<std::int32_t>(lineIndex + 1)) {
            result.hits.back().matches.push_back(local);
            continue;
        }
        LineHit hit;
        hit.line = static_cast<std::int32_t>(lineIndex + 1);
        // 行文本统一转成 UTF-8：界面层就不必再关心每个文件原本的编码
        hit.text = DocBytesToUtf8(LineTextAt(text, lineStarts, lineIndex), utfMode,
                                  loaded.encoding.codePage);
        hit.matches.push_back(local);
        result.hits.push_back(std::move(hit));
    }
    return result;
}

// 使用 std::filesystem 遍历；权限错误直接跳过而不是中断整次搜索。
void EnumerateFiles(const SearchConfig& config, const FileFilter& filter,
                    std::vector<std::wstring>& out, const CrossSearchJob& job) {
    std::error_code ec;
    for (const std::wstring& directory : config.directories) {
        if (job.IsCancelled() || out.size() >= config.maxFiles) {
            return;
        }
        std::filesystem::recursive_directory_iterator it(
            directory, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            continue;
        }
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (job.IsCancelled() || out.size() >= config.maxFiles) {
                return;
            }
            const std::filesystem::directory_entry& entry = *it;
            const std::wstring name = entry.path().filename().wstring();

            std::error_code typeError;
            if (entry.is_directory(typeError)) {
                if (!config.includeSubDirs || IsDefaultExcludedDir(name) ||
                    !FilterAllowsDirectory(filter, name)) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (typeError) {
                continue;
            }
            if (!FilterAllowsFile(filter, name)) {
                continue;
            }
            out.push_back(entry.path().wstring());
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- CrossSearchJob

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
    // 协调线程结束是 finished 置位的必要条件，但线程句柄仍需回收
    lock.unlock();
    if (coordinator_.joinable()) {
        coordinator_.join();
    }
}

// ---------------------------------------------------------------- CrossFileSearch

CrossFileSearch::CrossFileSearch(unsigned workerThreads) : pool_(workerThreads) {}
CrossFileSearch::~CrossFileSearch() = default;

std::shared_ptr<CrossSearchJob> CrossFileSearch::Start(const SearchConfig& config,
                                                       CrossSearchCallbacks callbacks) {
    auto job = std::make_shared<CrossSearchJob>();
    // 捕获 shared_ptr 让任务在线程运行期间保持存活
    job->coordinator_ = std::thread([this, job, config, callbacks]() {
        const FileFilter filter = ParseFileFilter(config.filter);
        SearchProgress progress;

        std::vector<std::wstring> files;
        EnumerateFiles(config, filter, files, *job);
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

            std::mutex                 resultsMutex;
            std::vector<FileSearchResult> results;
            std::mutex                 doneMutex;
            std::condition_variable    doneSignal;
            auto                       remaining =
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
