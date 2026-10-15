#include "infra/Paths.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

namespace npcc::infra {
namespace {

std::filesystem::path g_moduleDir;
std::filesystem::path g_configRoot;
bool g_portable = false;
bool g_initialized = false;

std::filesystem::path QueryModuleDir() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            break;
        }
        buffer.resize(buffer.size() * 2);   // 路径比缓冲区长，翻倍重试
    }
    std::filesystem::path exePath(buffer.data());
    return exePath.parent_path();
}

std::filesystem::path QueryRoamingAppData() {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw))) {
        return {};
    }
    std::filesystem::path result(raw);
    ::CoTaskMemFree(raw);
    return result;
}

// 用"能否在其中创建文件"来判断可写性，这比检查文件属性可靠。
bool DirectoryIsWritable(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    const auto probe = dir / L".write-probe";
    const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    ::CloseHandle(file);
    std::filesystem::remove(probe, ec);
    return true;
}

}  // namespace

void InitializePaths(bool forcePortable) {
    if (g_initialized) {
        return;
    }
    g_moduleDir = QueryModuleDir();

    bool portable = forcePortable;
    if (!portable) {
        // 标记文件优先于目录可写性判断：用户显式放一个 portable 文件就表示要便携
        std::error_code ec;
        portable = std::filesystem::exists(g_moduleDir / L"portable", ec);
    }
    if (!portable) {
        // 程序目录下存在可写的 config 目录时也视为便携（绿色解压即用的默认形态）
        portable = DirectoryIsWritable(g_moduleDir / L"config");
    }

    g_portable = portable;
    if (portable) {
        g_configRoot = g_moduleDir / L"config";
    } else {
        g_configRoot = QueryRoamingAppData() / L"notepadcc";
    }
    g_initialized = true;
}

const std::filesystem::path& ModuleDir() {
    return g_moduleDir;
}

bool IsPortableMode() {
    return g_portable;
}

const std::filesystem::path& ConfigRoot() {
    return g_configRoot;
}

std::filesystem::path LogDir() {
    return g_configRoot / L"logs";
}

std::filesystem::path BackupDir() {
    return g_configRoot / L"backup";
}

std::filesystem::path TempDir() {
    return g_configRoot / L"tmp";
}

std::wstring NormalizePath(std::wstring_view path) {
    if (path.empty()) {
        return {};
    }
    const std::wstring input(path);
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = ::GetFullPathNameW(input.c_str(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 buffer.data(), nullptr);
        if (written == 0) {
            return input;   // 规范化失败就原样返回，交给后续的错误处理
        }
        if (written < buffer.size()) {
            return std::wstring(buffer.data(), written);
        }
        buffer.resize(static_cast<size_t>(written) + 1);
    }
}

}  // namespace npcc::infra
