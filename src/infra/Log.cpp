#include "infra/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace npcc::infra {
namespace {

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
constexpr unsigned long long kMaxLogBytes = 4ull * 1024 * 1024;

const wchar_t* LevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return L"TRACE";
        case LogLevel::Info:  return L"INFO ";
        case LogLevel::Warn:  return L"WARN ";
        case LogLevel::Error: return L"ERROR";
    }
    return L"?    ";
}

// 日志文件统一以 UTF-8 写入，便于跨工具查看。
void WriteUtf8(HANDLE file, std::wstring_view text) {
    if (text.empty() || file == INVALID_HANDLE_VALUE) {
        return;
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return;
    }
    std::string buffer(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          buffer.data(), need, nullptr, nullptr);
    DWORD written = 0;
    ::WriteFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr);
}

std::wstring Timestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return Format(L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void RotateIfNeeded(const std::filesystem::path& logPath) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(logPath, ec);
    if (ec || size <= kMaxLogBytes) {
        return;
    }
    ::CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
    auto rotated = logPath;
    rotated += L".1";
    std::filesystem::remove(rotated, ec);
    std::filesystem::rename(logPath, rotated, ec);
    if (ec) {
        // 轮转失败就继续追加，别因为日志把主流程搞挂
        std::filesystem::remove(logPath, ec);
    }
}

}  // namespace

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, fmt);
    const int written = vswprintf(buffer, 2048, fmt, args);
    va_end(args);
    if (written < 0) {
        return {};
    }
    return std::wstring(buffer, static_cast<size_t>(written));
}

void InitLogging(const std::filesystem::path& logDir) {
    std::lock_guard<std::mutex> guard(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    const auto logPath = logDir / L"notepadcc.log";
    RotateIfNeeded(logPath);

    g_logFile = ::CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void ShutdownLogging() {
    std::lock_guard<std::mutex> guard(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

void LogMessage(LogLevel level, std::wstring_view category, std::wstring_view message) {
    std::lock_guard<std::mutex> guard(g_logMutex);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }
    // 必须用 %ls：宽字符 printf 里 %s 表示窄字符串（MinGW/标准 C 语义），
    // 直接写 %s 会把宽指针当 char* 读，日志只剩首字符。
    const std::wstring categoryText(category);
    std::wstring line = Format(L"[%ls] [%ls] [%ls] ", Timestamp().c_str(), LevelName(level),
                               categoryText.c_str());
    line.append(message);
    line.append(L"\r\n");
    WriteUtf8(g_logFile, line);
}

}  // namespace npcc::infra
