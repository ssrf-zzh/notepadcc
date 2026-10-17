#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace npcc::license {

// 版本（设计 2.1）：许可文件里的 edition 是权威值；无许可或校验失败一律 Free；
// 试用期内视为 Pro（状态机在 LicenseService，B7）。
enum class Edition {
    Free,
    Pro,
    Enterprise,
};

// 能力位（设计 2.2）：用位掩码而不是 N 个布尔——许可文件里 features= 一行能
// 表达全部付费点，以后增删也不动容器格式。
enum class Feature : std::uint32_t {
    MultiCaret    = 1u << 0,   // multicaret
    Compare       = 1u << 1,   // compare
    DataTools     = 1u << 2,   // datatools
    HugeFile      = 1u << 3,   // hugefile
    ScriptPlugin  = 1u << 4,   // scriptplugin
    RemoteEdit    = 1u << 5,   // remoteedit
    BatchCli      = 1u << 6,   // batchcli
    CloudSync     = 1u << 7,   // cloudsync
    TeamConfig    = 1u << 8,   // teamconfig（Enterprise 专属）
    ManagedDeploy = 1u << 9,   // manageddeploy（Enterprise 专属）
};

using FeatureMask = std::uint32_t;

constexpr FeatureMask FeatureBit(Feature feature) { return static_cast<FeatureMask>(feature); }
constexpr bool HasFeature(FeatureMask mask, Feature feature) {
    return (mask & FeatureBit(feature)) != 0;
}

// 名字 ↔ 位（名字就是许可文件 features= 里的记号，见设计 2.2 的表）
std::string_view FeatureName(Feature feature);
std::optional<Feature> FeatureFromName(std::string_view name);
// 位集合 → 按名字字典序的 "a,b,c"（签发端序列化用；解析端按 ',' 切开）
std::string FeatureListToString(FeatureMask mask);

// 试用期的能力集（设计 2.2）：Pro 全集——Enterprise 专属的两位不在里面。
FeatureMask ProFeatureMask();

std::string_view EditionName(Edition edition);
std::optional<Edition> EditionFromName(std::string_view name);

// 指纹策略（设计 4.2）：tolerant = 默认，四项里至少两项匹配放行；
// exact = 四项全中；none = 站点许可，不做机器判定。
enum class FingerprintMode {
    Tolerant,
    Exact,
    None,
};

std::string_view FingerprintModeName(FingerprintMode mode);
std::optional<FingerprintMode> FingerprintModeFromName(std::string_view name);

// 运行状态（设计 2.1 与第 5 章）：许可校验不过一律回到 Free——Free 是**正常**
// 状态，不是"出错"。判定顺序（B7）：许可有效 → Licensed；刚过期（7 天宽限内）
// → Grace（功能仍可用，提示续期）；没有可用许可但试用窗口没到期 → Trial（能力
// 集按 Pro 计）；其余 Free。
enum class LicenseState {
    Free,
    Trial,
    Grace,
    Licensed,
};

std::string_view LicenseStateName(LicenseState state);

// 许可错误（设计 9 的错误表）。文案分两路：DescribeLicenseError 给用户看的
// 一句话，LicenseErrorLevel 决定日志级别——"没有许可""过期"这类是 Info，
// 只有真异常才 Warn，Free 是正常状态，不该把日志刷满。
enum class LicenseError {
    None,
    FileMissing,              // 没找到许可文件（Free 是正常状态，不说话）
    IoError,                  // 文件在但读不出来
    MalformedContainer,       // 容器或 payload 结构不合法（含缺必填键、值不合法）
    ProductMismatch,          // product 不是本产品（防许可被挪用到别的产品）
    SignatureInvalid,         // 验签不过（文件被改过 / 签发错误）
    KeyUnknown,               // keyId 不在内置公钥表里
    SchemaTooNew,             // payload 由更新版本签发
    Expired,                  // 过了 expiresAt
    NotYetValid,              // 还没到 notBefore
    FingerprintMismatch,      // 许可绑的不是这台机器（B4 判定，这里先定义）
    ClockAnomaly,             // 本地时钟看起来被回拨过（B7 判定）
    ActivationNetworkFailed,  // 在线激活没连上服务器（B6 用，降级为离线指引）
};

enum class LicenseLogLevel {
    Info,
    Warn,
};

std::wstring DescribeLicenseError(LicenseError error);
LicenseLogLevel LicenseErrorLevel(LicenseError error);

// 错误名的机器可读写法（--license-info 的 JSON、日志与脚本比对用）。
// 用户看的文案走 DescribeLicenseError，两者不要互相替代。
std::string_view LicenseErrorName(LicenseError error);

// 解析出来的许可信息。签发端（tools/licgen，B12）也用同一个类型来序列化——
// 两端共用一套字段，避免格式漂移（设计 3.4）。
struct LicenseInfo {
    int schema = 0;
    std::string product;                  // 固定 notepadcc；序列化时空值按 notepadcc 写
    std::string licenseId;                // UUID（续费、停用、统计都认它）
    Edition edition = Edition::Free;
    unsigned int seats = 0;               // 0 = 站点 / 不限量（Enterprise 站点授权）
    std::string holder;
    std::string holderId;                 // 可选：统一社会信用代码 / 税号
    std::optional<std::int64_t> issuedAt; // epoch 秒（UTC），下同
    std::optional<std::int64_t> notBefore;
    std::optional<std::int64_t> expiresAt;
    std::optional<std::int64_t> updateUntil;
    FeatureMask features = 0;
    std::vector<std::string> unknownFeatures;   // features 里不认识的名字：不授权，
                                                // 但也不让整张许可作废（列出来给人看）
    std::string restrictions;             // 可选声明（如 noncommercial）
    FingerprintMode fingerprintMode = FingerprintMode::Tolerant;
    std::string fingerprint;              // 短码；fingerprintMode=none 时为空
    std::string channel;                  // stable / beta（空 = 不限制）
    std::string keyId;                    // 容器里的签发密钥标识（验证端填）
};

// ISO8601 UTC：严格 "YYYY-MM-DDThh:mm:ssZ"（设计 3.3：时间一律 UTC、不带偏移、
// 不带小数秒）。解析失败返回 nullopt；年份范围 1970–9999，闰年与月末都按真实
// 历法核对（2023-02-29 这种直接拒绝）。
std::optional<std::int64_t> ParseIso8601Utc(std::string_view text);
std::string FormatIso8601Utc(std::int64_t epochSeconds);

// 当前时间的 epoch 秒。验证与状态机一律显式接 now 参数，只有这里读系统时钟——
// 过期、宽限、时钟回拨这些判定才钉得住。
std::int64_t NowUnixUtc();

// 随机请求号（UUID v4 形状）：在线激活的 requestId 与离线申请串共用——16 字节
// 随机数按 UUID v4 打上版本位与变体位后转小写十六进制。随机源起不来时返回
// "unknown"：让服务端一眼看出这是异常件，而不是重用一个号。
std::string NewRequestId();

constexpr std::string_view kProductName = "notepadcc";
constexpr std::string_view kLicenseAlgName = "RSA-2048-PKCS1-SHA256";

}  // namespace npcc::license
