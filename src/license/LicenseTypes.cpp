#include "license/LicenseTypes.h"

#include <cstdio>
#include <ctime>

#include "infra/Crypto.h"

namespace npcc::license {
namespace {

// 特征表按**名字字典序**排：FeatureListToString 直接顺着表输出就是规范顺序
// （许可文件里 features= 的值因此是稳定的、可逐字节比对的）。
struct FeatureEntry {
    std::string_view name;
    Feature          feature;
};

constexpr FeatureEntry kFeatureTable[] = {
    {"batchcli", Feature::BatchCli},
    {"cloudsync", Feature::CloudSync},
    {"compare", Feature::Compare},
    {"datatools", Feature::DataTools},
    {"hugefile", Feature::HugeFile},
    {"manageddeploy", Feature::ManagedDeploy},
    {"multicaret", Feature::MultiCaret},
    {"remoteedit", Feature::RemoteEdit},
    {"scriptplugin", Feature::ScriptPlugin},
    {"teamconfig", Feature::TeamConfig},
};

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
    constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) {
        return 29;
    }
    return kDays[month - 1];
}

// civil → 天数（Howard Hinnant 的 days_from_civil；1970-01-01 为第 0 天）
std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - static_cast<int>(era * 400));
    const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void CivilFromDays(std::int64_t days, int* year, int* month, int* day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    *year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    *month = static_cast<int>(m);
    *day = static_cast<int>(d);
}

int TwoDigits(std::string_view text, std::size_t pos) {
    return (text[pos] - '0') * 10 + (text[pos + 1] - '0');
}

}  // namespace

std::string_view FeatureName(Feature feature) {
    for (const FeatureEntry& entry : kFeatureTable) {
        if (entry.feature == feature) {
            return entry.name;
        }
    }
    return {};
}

std::optional<Feature> FeatureFromName(std::string_view name) {
    for (const FeatureEntry& entry : kFeatureTable) {
        if (entry.name == name) {
            return entry.feature;
        }
    }
    return std::nullopt;
}

std::string FeatureListToString(FeatureMask mask) {
    std::string out;
    for (const FeatureEntry& entry : kFeatureTable) {
        if ((mask & FeatureBit(entry.feature)) == 0) {
            continue;
        }
        if (!out.empty()) {
            out.push_back(',');
        }
        out.append(entry.name);
    }
    return out;
}

std::string_view EditionName(Edition edition) {
    switch (edition) {
        case Edition::Pro: return "pro";
        case Edition::Enterprise: return "enterprise";
        case Edition::Free: break;
    }
    return "free";
}

std::optional<Edition> EditionFromName(std::string_view name) {
    if (name == "pro") {
        return Edition::Pro;
    }
    if (name == "enterprise") {
        return Edition::Enterprise;
    }
    return std::nullopt;
}

std::string_view FingerprintModeName(FingerprintMode mode) {
    switch (mode) {
        case FingerprintMode::Tolerant: return "tolerant";
        case FingerprintMode::Exact: return "exact";
        case FingerprintMode::None: break;
    }
    return "none";
}

std::optional<FingerprintMode> FingerprintModeFromName(std::string_view name) {
    if (name == "tolerant") {
        return FingerprintMode::Tolerant;
    }
    if (name == "exact") {
        return FingerprintMode::Exact;
    }
    if (name == "none") {
        return FingerprintMode::None;
    }
    return std::nullopt;
}

std::string_view LicenseStateName(LicenseState state) {
    switch (state) {
        case LicenseState::Trial: return "trial";
        case LicenseState::Licensed: return "licensed";
        case LicenseState::Free: break;
    }
    return "free";
}

std::wstring DescribeLicenseError(LicenseError error) {
    switch (error) {
        case LicenseError::None: return L"许可有效";
        case LicenseError::FileMissing: return L"没有找到许可文件（Free 是正常状态）";
        case LicenseError::IoError: return L"许可文件读不出来（权限或磁盘问题）";
        case LicenseError::MalformedContainer: return L"许可文件格式不正确";
        case LicenseError::ProductMismatch: return L"这不是本产品的许可（product 字段不匹配）";
        case LicenseError::SignatureInvalid:
            return L"许可签名校验不通过，请确认文件未被修改";
        case LicenseError::KeyUnknown:
            return L"本版本不认识该许可的签发密钥，请升级 notepadcc";
        case LicenseError::SchemaTooNew: return L"许可由更新版本签发，请升级 notepadcc";
        case LicenseError::Expired: return L"许可已过期";
        case LicenseError::NotYetValid: return L"许可尚未生效";
        case LicenseError::FingerprintMismatch:
            return L"许可绑定的不是这台机器，请按换绑指引处理";
        case LicenseError::ClockAnomaly: return L"系统时间看起来被回拨过，许可判定按保守值处理";
        case LicenseError::ActivationNetworkFailed:
            return L"在线激活没有连上服务器，请改用离线激活";
    }
    return L"未知的许可错误";
}

LicenseLogLevel LicenseErrorLevel(LicenseError error) {
    switch (error) {
        // 正常状态与非异常路径：留一条 Info 就够，不打扰人
        case LicenseError::None:
        case LicenseError::FileMissing:
        case LicenseError::Expired:
        case LicenseError::NotYetValid:
        case LicenseError::FingerprintMismatch:
        case LicenseError::ActivationNetworkFailed:
            return LicenseLogLevel::Info;
        default:
            // 格式不对、验签不过、密钥不认识、schema 太新、时钟异常：值得 Warn
            return LicenseLogLevel::Warn;
    }
}

std::string_view LicenseErrorName(LicenseError error) {
    switch (error) {
        case LicenseError::None: return "none";
        case LicenseError::FileMissing: return "fileMissing";
        case LicenseError::IoError: return "ioError";
        case LicenseError::MalformedContainer: return "malformedContainer";
        case LicenseError::ProductMismatch: return "productMismatch";
        case LicenseError::SignatureInvalid: return "signatureInvalid";
        case LicenseError::KeyUnknown: return "keyUnknown";
        case LicenseError::SchemaTooNew: return "schemaTooNew";
        case LicenseError::Expired: return "expired";
        case LicenseError::NotYetValid: return "notYetValid";
        case LicenseError::FingerprintMismatch: return "fingerprintMismatch";
        case LicenseError::ClockAnomaly: return "clockAnomaly";
        case LicenseError::ActivationNetworkFailed: return "activationNetworkFailed";
    }
    return "unknown";
}

std::optional<std::int64_t> ParseIso8601Utc(std::string_view text) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return std::nullopt;
    }
    for (const std::size_t index : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
    }
    const int year = TwoDigits(text, 0) * 100 + TwoDigits(text, 2);
    const int month = TwoDigits(text, 5);
    const int day = TwoDigits(text, 8);
    const int hour = TwoDigits(text, 11);
    const int minute = TwoDigits(text, 14);
    const int second = TwoDigits(text, 17);
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month) || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    return DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
           hour * 3600 + minute * 60 + second;
}

std::string FormatIso8601Utc(std::int64_t epochSeconds) {
    std::int64_t days = epochSeconds / 86400;
    std::int64_t rest = epochSeconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    CivilFromDays(days, &year, &month, &day);
    const int hour = static_cast<int>(rest / 3600);
    const int minute = static_cast<int>((rest % 3600) / 60);
    const int second = static_cast<int>(rest % 60);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day,
                  hour, minute, second);
    return buffer;
}

std::int64_t NowUnixUtc() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string NewRequestId() {
    std::uint8_t bytes[16] = {};
    if (!infra::RandomBytes(bytes, sizeof(bytes))) {
        return "unknown";   // 随机源都起不来：给个显眼的值，别悄悄重用一个号
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);   // 版本 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);   // 变体 10
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[bytes[index] >> 4]);
        out.push_back(kHex[bytes[index] & 0x0F]);
    }
    return out;
}

}  // namespace npcc::license
