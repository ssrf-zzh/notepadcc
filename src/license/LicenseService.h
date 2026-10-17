#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "license/Fingerprint.h"
#include "license/LicenseStore.h"
#include "license/LicenseTypes.h"
#include "license/LicenseVerifier.h"

namespace npcc::license {

// 许可判定的结果（纯函数）：把"验签通过的许可内容"落到"运行状态"上。
// machine 显式传入——判定钉得住，测试也不必真的动机器（与 Match 同一思路）。
struct LicenseEvaluation {
    LicenseState state = LicenseState::Free;
    LicenseError error = LicenseError::None;
    FingerprintMatch fingerprint;   // 机器判定明细（mode=none 时是 NotBound）
};

LicenseEvaluation EvaluateLicense(const LicenseVerifyResult& verified, const Fingerprint& machine);

// ---- 试用与回落（B7，《notepadcc许可与激活设计.md》2.2 / 第 5 章）----

constexpr std::int64_t kTrialDays = 30;                    // 试用窗口（天）
constexpr std::int64_t kGraceDays = 7;                     // 到期后的宽限（天）
constexpr std::int64_t kClockBackstepSeconds = 24 * 3600;  // 回拨超过一天才算异常

// 试用窗口判定（纯函数，now 显式）：回拨时剩余天数按 max(now, lastSeenUtc) 算，
// 回拨不产生额外天数；daysLeft 向上取整——没到期就不报 0。
struct TrialEvaluation {
    bool started = false;       // 有首次运行记录（第一次运行写下记录之前是 false）
    bool active = false;        // 首次运行 + 30 天 > 有效现在
    int  daysLeft = 0;          // 0..30
    bool clockAnomaly = false;  // now 比上次运行早超过 24 小时
};
TrialEvaluation EvaluateTrial(const TrialRecord& record, std::int64_t now);

// 宽限期还剩几天（纯函数）：0 = 没有到期时间 / 还没到期 / 已经出了宽限。
int RemainingGraceDays(const LicenseInfo& info, std::int64_t now);

// 会话级判定（纯函数）：验签结果 + 机器 + 试用记录 + now → 运行状态与能力集。
// 许可有效 → Licensed；刚过期（宽限内）→ Grace（功能仍可用）；没有可用许可但
// 试用没到期 → Trial（能力集按 Pro 全算计）；其余 Free。
struct SessionEvaluation {
    LicenseState state = LicenseState::Free;
    LicenseError error = LicenseError::None;
    FeatureMask features = 0;             // Free 时恒为 0
    FingerprintMatch fingerprint;         // 机器判定明细（mode=none 时是 NotBound）
    int trialDaysLeft = 0;
    int graceDaysLeft = 0;
    bool clockAnomaly = false;
    bool trialActive = false;             // 试用窗口内（不管当前用没用上）
};
SessionEvaluation EvaluateSession(const LicenseVerifyResult& verified, const Fingerprint& machine,
                                  const TrialRecord& record, std::int64_t now);

// 启动提示（设计 5.3 / 第 9 章）：一次会话至多露面一次；勾"不再提示"写回 state.xml。
enum class LicenseNotice {
    None,
    TrialExpired,   // 试用到期回落：启动时提示一次，可勾不再提示
    Grace,          // 许可过期、还在宽限期：提示续期（可勾不再提示）
    ClockAnomaly,   // 时钟回拨：仅试用状态，一次会话一次
};

struct StartupNotice {
    LicenseNotice kind = LicenseNotice::None;
    int daysLeft = 0;                       // Grace：宽限剩余天数
    std::string holder;                     // Grace：持有人（UTF-8，可为空）
    std::optional<std::int64_t> expiresAt;  // Grace：到期时间
};

// 选哪条提示（纯函数）：宽限 > 试用到期 > 时钟异常；两个 dismissed 标记与
// "本会话已露过"的记账都在这里判掉。
StartupNotice SelectStartupNotice(const LicenseInfo& info, LicenseState state,
                                  const TrialRecord& record, bool trialActive, int graceDaysLeft,
                                  bool trialShown, bool graceShown, bool clockShown,
                                  bool clockAnomaly);

// 开始一次运行会话的输入（测试注入点）：stateFile 空 = ConfigRoot()/state.xml；
// registrySubKey 空 = 不碰注册表（单测在测试机上不留痕）。
struct TrialEnvironment {
    std::filesystem::path stateFile;
    std::wstring registrySubKey;
};

// JSON 字符串转义（--license-info 的输出、对话框"复制诊断信息"都用它）：
// 引号、反斜杠与控制字符按 RFC 8259 转义，其余原样（含 UTF-8 中文）。
std::string JsonEscape(std::string_view text);

// 许可服务的唯一入口（设计 8.1 / 8.3）：启动时读一次本地文件 + 验签 + 机器判定，
// 失败只记日志、**绝不阻断启动**；界面对许可的所有查询都走这里。单例是进程级的，
// 启动与界面都在 UI 线程，因此不加锁（在线激活 B6 要走后台线程时再引入同步）。
class LicenseService {
public:
    static LicenseService& Instance();

    // licenseFile 为空指针时用默认位置（ConfigRoot()/license.dat）。
    void Initialize(const std::filesystem::path* licenseFile);

    // 开始一次运行会话（B7）：更新双写记录（state.xml + HKCU）、记 lastSeenUtc、
    // 之后重算状态。写盘失败只记日志——试用记账绝不能拦住启动。
    void BeginSession(const TrialEnvironment* environment = nullptr);

    // 重新读盘 + 验签 + 机器判定（导入之后要立即生效）。返回是否处于已激活。
    bool Reload();

    LicenseState State() const { return state_; }
    Edition EditionValue() const;              // Free / Pro / Enterprise
    bool Has(Feature feature) const;           // 能力门禁只认这一个（Free 一律 false）
    FeatureMask Features() const { return features_; }
    const LicenseInfo& Info() const { return info_; }         // 过期 / 未生效时也留着内容
    LicenseError LastError() const { return error_; }
    const std::wstring& LastDetail() const { return detail_; }
    FingerprintMatch FingerprintResult() const { return fingerprint_; }
    std::int64_t CheckedAtUnix() const { return checkedAt_; }
    const std::filesystem::path& Path() const { return path_; }

    // ---- 试用与回落（B7）----
    int  TrialDaysLeft() const { return trialDaysLeft_; }
    int  GraceDaysLeft() const { return graceDaysLeft_; }
    bool ClockAnomaly() const { return clockAnomaly_; }

    // 启动提示：本次会话该露哪一面；"露过了"与"不再提示"的记账也在这里。
    StartupNotice PendingStartupNotice() const;
    void MarkNoticeShown(LicenseNotice kind);
    bool DismissNoticeForever(LicenseNotice kind);   // 勾"不再提示"：写回 state.xml

    // 导入（设计 3.5）：源文件先当作许可验一遍，"结构上是一张真许可"才写到
    // Path()，然后立即重新加载。**过期 / 未生效 / 指纹不符不影响导入成功**——
    // 那些是"这张许可现在用不上"，不是"这不是一张许可"；企业 IT 批量推文件时
    // 更希望退出码只回答后者（前者用 --license-info 查）。
    bool Import(const std::filesystem::path& source, LicenseError* error, std::wstring* detail);

    // 从内存导入（B6 在线激活与离线对话框共用出口）：网络来的字节按同一门槛
    // 防一遍——“结构上是真许可”才落盘，成功后立即 Reload。
    bool ImportBytes(std::string_view bytes, LicenseError* error, std::wstring* detail);

    // 离线申请串（设计 7.1）：product / requestId / fingerprint / appVersion /
    // seats / holder，纯文本、LF 行尾。seats 与 holder 由界面收集（客户填写）；
    // 指纹现场重采一次（比缓存诚实：程序运行期间也可能换硬件）。
    std::string ExportChallenge(std::string_view appVersion, int seats,
                                std::string_view holderUtf8) const;

    // --license-info 的输出（设计 8.5）：一行 JSON。
    std::string StatusJson() const;

private:
    LicenseService() = default;
    LicenseService(const LicenseService&) = delete;
    LicenseService& operator=(const LicenseService&) = delete;

    void LogStatus() const;

    LicenseState     state_ = LicenseState::Free;
    LicenseError     error_ = LicenseError::FileMissing;
    std::wstring     detail_;
    LicenseInfo      info_;
    FeatureMask      features_ = 0;
    FingerprintMatch fingerprint_;
    std::int64_t     checkedAt_ = 0;
    std::filesystem::path path_;

    // ---- 试用与回落（B7）----
    TrialRecord          record_;
    std::filesystem::path statePath_;
    std::wstring         registrySubKey_;
    bool                 sessionStarted_ = false;
    bool                 trialActive_ = false;
    int                  trialDaysLeft_ = 0;
    int                  graceDaysLeft_ = 0;
    bool                 clockAnomaly_ = false;
    bool                 noticeTrialShown_ = false;
    bool                 noticeGraceShown_ = false;
    bool                 noticeClockShown_ = false;
};

}  // namespace npcc::license
