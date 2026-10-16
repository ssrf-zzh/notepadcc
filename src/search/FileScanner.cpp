#include "search/FileScanner.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>

#include "edit/Encoding.h"
#include "io/FileReader.h"
#include "search/Wildcard.h"

namespace npcc::search {
namespace {

constexpr std::uint32_t kFallbackCodePage = 936;

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

}  // namespace

void EnumerateFiles(const SearchConfig& config, std::vector<std::wstring>& out,
                    const std::function<bool()>& cancelled) {
    const FileFilter filter = ParseFileFilter(config.filter);
    std::error_code ec;
    for (const std::wstring& directory : config.directories) {
        if (out.size() >= config.maxFiles || (cancelled && cancelled())) {
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
            if (out.size() >= config.maxFiles || (cancelled && cancelled())) {
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

bool PrepareFile(const std::wstring& path, const SearchConfig& config, PreparedFile& out) {
    out = PreparedFile{};

    const io::ReadResult read = io::ReadWholeFile(path, config.maxFileSizeBytes);
    if (!read.ok) {
        out.error = read.message;
        return false;
    }
    out.fileSize = read.size;
    QueryFileIdentity(path, out.fileSize, out.lastWriteMs);

    if (config.skipBinary && edit::LooksBinaryContent(read.bytes)) {
        out.binary = true;
        return false;
    }

    const edit::LoadResult loaded = edit::DetectAndPrepare(read.bytes, kFallbackCodePage);
    out.bytes = loaded.editorBytes;
    out.utfMode = loaded.encoding.uniMode != edit::UniMode::Ansi;
    out.codePage = loaded.encoding.codePage;
    return true;
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

}  // namespace npcc::search
