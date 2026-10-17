#pragma once

#include <filesystem>
#include <optional>
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

// ---- 试用记录（B7，设计 5.1 / 5.2）----

// 双写记录：state.xml 持全部字段；HKCU 只持"首次运行时间"——挡"删掉配置
// 文件重来一次"，尽力而为（设计 1.2 明说：不做隐藏目录、不做对抗）。
struct TrialRecord {
    std::optional<std::int64_t> firstRunUtc;   // 首次运行（双写取较早）
    std::optional<std::int64_t> lastSeenUtc;   // 上次运行（时钟回拨判定用）
    bool trialNoticeDismissed = false;         // "试用已到期"提示：勾过不再提示
    bool graceNoticeDismissed = false;         // "宽限期"提示：勾过不再提示
};

// state.xml 的默认位置：ConfigRoot()/state.xml（便携模式跟着程序走）。
std::filesystem::path DefaultTrialStatePath();

// 读 state.xml：文件不存在 = 空记录（不是错误，第一次运行就是这样）；
// 解析失败返回 false——调用方按空记录处理并记日志，试用判定不能因为一个
// 坏文件把程序拦下。
bool LoadTrialState(const std::filesystem::path& path, TrialRecord& out,
                    std::wstring* detail);
bool SaveTrialState(const std::filesystem::path& path, const TrialRecord& record,
                    std::wstring* detail);

// HKCU 那一路（REG_QWORD）。读不到（没写过 / 权限不够）返回 nullopt；
// 空 subKey = 明确跳过注册表（单测用，测试机器上不留痕）。
std::optional<std::int64_t> ReadRegistryFirstRun(std::wstring_view subKey);
bool WriteRegistryFirstRun(std::int64_t value, std::wstring_view subKey);

// 合并双写（纯函数）：firstRun 两处取较早，其余字段以文件为准。
TrialRecord MergeTrialRecords(const TrialRecord& file,
                            std::optional<std::int64_t> registryFirstRun);

}  // namespace npcc::license
