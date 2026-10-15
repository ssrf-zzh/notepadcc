#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace npcc::infra {

enum class LogLevel { Trace, Info, Warn, Error };

// 在指定的日志目录下建立 notepadcc.log；超过 4 MB 时轮转为 notepadcc.1.log。
// 重复调用无副作用。写日志在调用线程内完成（M0 尚未引入日志线程）。
void InitLogging(const std::filesystem::path& logDir);
void ShutdownLogging();

void LogMessage(LogLevel level, std::wstring_view category, std::wstring_view message);

// 简易宽字符格式化，避免对 <format> 的实现差异产生依赖。
std::wstring Format(const wchar_t* fmt, ...);

inline void LogInfo(std::wstring_view category, std::wstring_view message) {
    LogMessage(LogLevel::Info, category, message);
}
inline void LogWarn(std::wstring_view category, std::wstring_view message) {
    LogMessage(LogLevel::Warn, category, message);
}
inline void LogError(std::wstring_view category, std::wstring_view message) {
    LogMessage(LogLevel::Error, category, message);
}

}  // namespace npcc::infra
