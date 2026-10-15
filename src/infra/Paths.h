#pragma once

#include <filesystem>

namespace npcc::infra {

// 解析并缓存程序目录、配置根目录与便携模式判定结果。
// forcePortable 来自命令行 -portable，优先级最高。
void InitializePaths(bool forcePortable);

const std::filesystem::path& ModuleDir();
bool IsPortableMode();

// 便携模式 = 程序目录/config；安装模式 = %APPDATA%\notepadcc
const std::filesystem::path& ConfigRoot();
std::filesystem::path LogDir();
std::filesystem::path BackupDir();
std::filesystem::path TempDir();

}  // namespace npcc::infra
