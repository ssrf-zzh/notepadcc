#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "license/LicenseTypes.h"

namespace npcc::license {

// 许可文件的默认位置（设计 3.5）：`ConfigRoot()/license.dat`——便携模式落在
// 程序目录、安装模式落在 %APPDATA%\notepadcc。
std::filesystem::path DefaultLicensePath();

// 读整个许可文件。文件不存在 → FileMissing（Free 是正常状态，不打扰人）；
// 存在但读不出来（权限 / 太长）→ IoError。
bool LoadLicenseFile(const std::filesystem::path& path, std::string& bytes,
                     LicenseError* error, std::wstring* detail);

// 写许可文件：目录不存在先建，内容走 io::WriteFileAtomic——不能出现半截的
// 许可文件（设计 3.5 与设计说明书 4.6 同一条契约）。
bool SaveLicenseFile(const std::filesystem::path& path, std::string_view bytes,
                     LicenseError* error, std::wstring* detail);

}  // namespace npcc::license