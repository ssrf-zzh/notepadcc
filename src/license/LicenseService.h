#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "license/Fingerprint.h"
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
};

}  // namespace npcc::license
