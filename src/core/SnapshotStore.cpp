#include "core/SnapshotStore.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "infra/Log.h"
#include "infra/Paths.h"
#include "io/AtomicFile.h"

namespace npcc::core {
namespace {

constexpr const char* kMagic = "notepadcc-snapshot";
constexpr int         kVersion = 1;

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                          size, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          size);
    return out;
}

// 无上限地把整个文件读进来。快照是我们自己刚写下的，不需要按大文件策略设限；
// 但也不会去读一个明显不是快照的巨型文件（头部解析会先失败）。
bool ReadAllBytes(const std::filesystem::path& path, std::string& out) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == 0 || size.QuadPart < 0) {
        ::CloseHandle(file);
        return false;
    }
    out.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t done = 0;
    while (done < out.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 20));
        if (::ReadFile(file, out.data() + done, want, &read, nullptr) == 0 || read == 0) {
            ::CloseHandle(file);
            return false;
        }
        done += read;
    }
    ::CloseHandle(file);
    return true;
}

bool ReadHeadBytes(const std::filesystem::path& path, std::string& out, std::size_t limit) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    out.assign(limit, '\0');
    DWORD read = 0;
    const BOOL ok = ::ReadFile(file, out.data(), static_cast<DWORD>(limit), &read, nullptr);
    ::CloseHandle(file);
    if (ok == 0) {
        return false;
    }
    out.resize(read);
    return true;
}

}  // namespace

std::filesystem::path SnapshotStore::BackupRoot() {
    return infra::BackupDir();
}

std::filesystem::path SnapshotStore::AutosaveDir() {
    return BackupRoot() / L"autosave";
}

std::filesystem::path SnapshotStore::CrashDirFor(std::wstring_view stamp) {
    return BackupRoot() / (L"crash_" + std::wstring(stamp));
}

std::wstring SnapshotStore::Timestamp() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t buffer[32] = {};
    ::swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::wstring SnapshotStore::DisplayTime() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t buffer[32] = {};
    ::swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::string SnapshotStore::Build(const SnapshotInfo& info, std::string_view editorBytes) {
    std::string out;
    out += kMagic;
    out += ' ';
    out += std::to_string(kVersion);
    out += "\r\n";
    out += "name=" + ToUtf8(info.displayName) + "\r\n";
    out += "path=" + ToUtf8(info.originalPath) + "\r\n";
    out += std::string("untitled=") + (info.untitled ? "yes" : "no") + "\r\n";
    out += std::string("uniMode=") + edit::UniModeToken(info.encoding.uniMode) + "\r\n";
    out += "codePage=" + std::to_string(info.encoding.codePage) + "\r\n";
    out += std::string("hasBom=") + (info.encoding.hasBom ? "yes" : "no") + "\r\n";
    out += std::string("eol=") + edit::EolToken(info.eol) + "\r\n";
    out += "savedAt=" + ToUtf8(info.savedAt.empty() ? DisplayTime() : info.savedAt) + "\r\n";
    out += "length=" + std::to_string(editorBytes.size()) + "\r\n";
    out += "\r\n";
    out.append(editorBytes.data(), editorBytes.size());
    return out;
}

bool SnapshotStore::ParseHeader(std::string_view raw, SnapshotInfo& info,
                                std::size_t& payloadOffset) {
    payloadOffset = 0;
    const std::size_t firstLineEnd = raw.find('\n');
    if (firstLineEnd == std::string_view::npos) {
        return false;
    }
    std::string_view firstLine = raw.substr(0, firstLineEnd);
    if (!firstLine.empty() && firstLine.back() == '\r') {
        firstLine.remove_suffix(1);
    }
    if (firstLine.compare(0, std::char_traits<char>::length(kMagic), kMagic) != 0) {
        return false;   // 不是我们的快照文件
    }

    SnapshotInfo parsed;
    bool haveLength = false;
    std::size_t pos = firstLineEnd + 1;
    while (pos <= raw.size()) {
        const std::size_t lineEnd = raw.find('\n', pos);
        if (lineEnd == std::string_view::npos) {
            return false;   // 头部没有以空行收尾 = 文件被截断
        }
        std::string_view line = raw.substr(pos, lineEnd - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = lineEnd + 1;
        if (line.empty()) {
            payloadOffset = pos;
            break;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);
        if (key == "name") {
            parsed.displayName = FromUtf8(value);
        } else if (key == "path") {
            parsed.originalPath = FromUtf8(value);
        } else if (key == "untitled") {
            parsed.untitled = value == "yes";
        } else if (key == "uniMode") {
            edit::UniMode mode = parsed.encoding.uniMode;
            if (!edit::ParseUniModeToken(value, mode)) {
                return false;   // 认不出的编码标识：宁可当坏文件，也不要猜错编码
            }
            parsed.encoding.uniMode = mode;
        } else if (key == "codePage") {
            parsed.encoding.codePage = static_cast<std::uint32_t>(std::strtoul(
                std::string(value).c_str(), nullptr, 10));
        } else if (key == "hasBom") {
            parsed.encoding.hasBom = value == "yes";
        } else if (key == "eol") {
            edit::EolMode eol = parsed.eol;
            if (!edit::ParseEolToken(value, eol)) {
                return false;
            }
            parsed.eol = eol;
        } else if (key == "savedAt") {
            parsed.savedAt = FromUtf8(value);
        } else if (key == "length") {
            parsed.length = std::strtoull(std::string(value).c_str(), nullptr, 10);
            haveLength = true;
        }
        // 不认识的键直接忽略：以后加字段时旧版本也能读
    }
    if (payloadOffset == 0 || !haveLength) {
        return false;
    }
    info = parsed;
    return true;
}

bool SnapshotStore::Parse(std::string_view raw, SnapshotInfo& info, std::string& editorBytes) {
    std::size_t offset = 0;
    if (!ParseHeader(raw, info, offset)) {
        return false;
    }
    const std::size_t available = raw.size() - offset;
    if (available != info.length) {
        return false;   // 内容长度与头部对不上：文件被截断或改坏了
    }
    editorBytes.assign(raw.data() + offset, available);
    return true;
}

bool SnapshotStore::Write(const SnapshotInfo& info, std::string_view editorBytes,
                          std::wstring* error) const {
    std::error_code ec;
    std::filesystem::create_directories(info.file.parent_path(), ec);
    const io::WriteResult result = io::WriteFileAtomic(info.file.wstring(),
                                                       Build(info, editorBytes));
    if (!result.ok) {
        if (error != nullptr) {
            *error = result.message;
        }
        return false;
    }
    return true;
}

std::vector<SnapshotInfo> SnapshotStore::List() const {
    std::vector<SnapshotInfo> found;
    std::error_code ec;

    auto scan = [&found](const std::filesystem::path& dir) {
        std::error_code inner;
        if (!std::filesystem::is_directory(dir, inner)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, inner)) {
            if (!entry.is_regular_file(inner) || entry.path().extension() != L".snap") {
                continue;
            }
            std::string head;
            if (!ReadHeadBytes(entry.path(), head, 4096)) {
                continue;
            }
            SnapshotInfo info;
            std::size_t offset = 0;
            if (!ParseHeader(head, info, offset)) {
                infra::LogWarn(L"snapshot", L"快照文件无法解析，已跳过: " + entry.path().wstring());
                continue;
            }
            info.file = entry.path();
            found.push_back(std::move(info));
        }
    };

    scan(AutosaveDir());
    if (std::filesystem::is_directory(BackupRoot(), ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(BackupRoot(), ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"crash_", 0) == 0) {
                scan(entry.path());
            }
        }
    }

    // 新的排前面：savedAt 是 "YYYY-MM-DD HH:MM:SS"，字典序即时间序
    std::sort(found.begin(), found.end(), [](const SnapshotInfo& a, const SnapshotInfo& b) {
        return a.savedAt > b.savedAt;
    });
    return found;
}

bool SnapshotStore::Read(const SnapshotInfo& info, std::string& editorBytes,
                         std::wstring* error) const {
    std::string raw;
    if (!ReadAllBytes(info.file, raw)) {
        if (error != nullptr) {
            *error = L"读取快照失败: " + info.file.wstring();
        }
        return false;
    }
    SnapshotInfo parsed;
    if (!Parse(raw, parsed, editorBytes)) {
        if (error != nullptr) {
            *error = L"快照内容已损坏: " + info.file.wstring();
        }
        return false;
    }
    return true;
}

bool SnapshotStore::Remove(const SnapshotInfo& info) const {
    std::error_code ec;
    return std::filesystem::remove(info.file, ec);
}

void SnapshotStore::RemoveAllAutosave() const {
    std::error_code ec;
    std::filesystem::remove_all(AutosaveDir(), ec);
}

void SnapshotStore::PruneCrashDirs(std::size_t keep) const {
    if (keep == 0) {
        keep = 1;
    }
    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    if (!std::filesystem::is_directory(BackupRoot(), ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(BackupRoot(), ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"crash_", 0) == 0) {
            dirs.push_back(entry.path());
        }
    }
    if (dirs.size() <= keep) {
        return;
    }
    // 名字里带时间戳，字典序即新旧
    std::sort(dirs.begin(), dirs.end());
    for (std::size_t i = 0; i + keep < dirs.size(); ++i) {
        std::error_code inner;
        std::filesystem::remove_all(dirs[i], inner);
        infra::LogInfo(L"snapshot", L"清理过期的崩溃快照目录: " + dirs[i].wstring());
    }
}

}  // namespace npcc::core
