#include "license/LicenseService.h"

#include <cstdio>
#include <utility>

#include "infra/Log.h"
#include "infra/TextCodec.h"
#include "license/LicenseStore.h"

namespace npcc::license {
namespace {

std::string NarrowPath(const std::filesystem::path& path) {
    return infra::Utf8FromWide(path.wstring());
}

// 日志里的一句话。界面文案不在这里——它走语言包（B6），日志与界面是两条线：
// 日志是给排查用的，用中文写清楚就够了。
std::wstring StatusLine(LicenseState state, LicenseError error, const LicenseInfo& info,
                        const std::wstring& detail) {
    if (state != LicenseState::Free) {
        std::wstring line = (state == LicenseState::Trial) ? L"许可：试用中" : L"许可：已激活";
        line += L"，版次 " + infra::WideFromUtf8(EditionName(info.edition));
        if (!info.holder.empty()) {
            line += L"，持有人「" + infra::WideFromUtf8(info.holder) + L"」";
        }
        line += L"，";
        if (info.expiresAt.has_value()) {
            line += L"到期 " + infra::WideFromUtf8(FormatIso8601Utc(*info.expiresAt));
        } else {
            line += L"永久";
        }
        if (!info.keyId.empty()) {
            line += L"，密钥 " + infra::WideFromUtf8(info.keyId);
        }
        return line;
    }
    std::wstring line = L"许可：Free（" + DescribeLicenseError(error) + L"）";
    if (!detail.empty()) {
        line += L" — " + detail;
    }
    return line;
}

}  // namespace

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8] = {};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X",
                                  static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                    out += buffer;
                } else {
                    out.push_back(ch);   // 中文等 UTF-8 字节原样输出，JSON 里照样可读
                }
                break;
        }
    }
    return out;
}

LicenseEvaluation EvaluateLicense(const LicenseVerifyResult& verified, const Fingerprint& machine) {
    LicenseEvaluation evaluation;
    if (!verified.ok()) {
        // 过期 / 未生效 / 格式 / 验签：状态一律 Free，错误照原样带出来
        evaluation.error = verified.error;
        return evaluation;
    }
    evaluation.fingerprint =
        Match(machine, verified.info.fingerprint, verified.info.fingerprintMode);
    if (!evaluation.fingerprint.Allows()) {
        evaluation.error = LicenseError::FingerprintMismatch;
        return evaluation;
    }
    evaluation.state = LicenseState::Licensed;
    return evaluation;
}

LicenseService& LicenseService::Instance() {
    static LicenseService instance;
    return instance;
}

void LicenseService::Initialize(const std::filesystem::path* licenseFile) {
    path_ = (licenseFile != nullptr && !licenseFile->empty()) ? *licenseFile : DefaultLicensePath();
    Reload();
}

bool LicenseService::Reload() {
    checkedAt_ = NowUnixUtc();
    state_ = LicenseState::Free;
    error_ = LicenseError::None;
    detail_.clear();
    info_ = LicenseInfo{};
    features_ = 0;
    fingerprint_ = FingerprintMatch{};

    std::string bytes;
    if (!LoadLicenseFile(path_, bytes, &error_, &detail_)) {
        LogStatus();
        return false;
    }
    const LicenseVerifyResult verified = VerifyLicenseBytes(bytes, checkedAt_);
    const LicenseEvaluation evaluation = EvaluateLicense(verified, Current());
    fingerprint_ = evaluation.fingerprint;
    error_ = evaluation.error;
    detail_ = verified.detail;
    info_ = verified.info;   // 过期 / 未生效 / 指纹不符也留着内容：界面要说清"谁的许可"
    if (evaluation.state != LicenseState::Free) {
        state_ = evaluation.state;
        features_ = info_.features;
    }
    LogStatus();
    return state_ != LicenseState::Free;
}

void LicenseService::LogStatus() const {
    const std::wstring line = StatusLine(state_, error_, info_, detail_);
    if (LicenseErrorLevel(error_) == LicenseLogLevel::Warn) {
        infra::LogWarn(L"license", line);
    } else {
        infra::LogInfo(L"license", line);
    }
}

Edition LicenseService::EditionValue() const {
    if (state_ == LicenseState::Free) {
        return Edition::Free;
    }
    return info_.edition;
}

bool LicenseService::Has(Feature feature) const {
    if (state_ == LicenseState::Free) {
        return false;   // 没有有效许可（含过期、指纹不符）一律不授权
    }
    return HasFeature(features_, feature);
}

bool LicenseService::Import(const std::filesystem::path& source, LicenseError* error,
                            std::wstring* detail) {
    std::string bytes;
    LicenseError readError = LicenseError::None;
    std::wstring readDetail;
    if (!LoadLicenseFile(source, bytes, &readError, &readDetail)) {
        if (error != nullptr) {
            *error = readError;
        }
        if (detail != nullptr) {
            *detail = std::move(readDetail);
        }
        return false;
    }
    if (!ImportBytes(bytes, error, detail)) {
        return false;
    }
    if (detail != nullptr) {
        *detail += L" — 已导入 " + source.wstring();
    }
    return true;
}

bool LicenseService::ImportBytes(std::string_view bytes, LicenseError* error,
                                 std::wstring* detail) {
    const auto report = [error, detail](LicenseError value, std::wstring text) {
        if (error != nullptr) {
            *error = value;
        }
        if (detail != nullptr) {
            *detail = std::move(text);
        }
    };

    const LicenseVerifyResult verified = VerifyLicenseBytes(bytes, NowUnixUtc());
    if (!verified.ok() && verified.error != LicenseError::Expired &&
        verified.error != LicenseError::NotYetValid) {
        // 结构 / 验签 / 字段有问题：这不是一张许可，绝不覆盖当前位置上的那一张
        report(verified.error,
               verified.detail.empty() ? DescribeLicenseError(verified.error) : verified.detail);
        return false;
    }
    LicenseError saveError = LicenseError::None;
    std::wstring saveDetail;
    if (!SaveLicenseFile(path_, bytes, &saveError, &saveDetail)) {
        report(saveError, saveDetail);
        return false;
    }
    Reload();   // 导入即生效
    report(LicenseError::None, StatusLine(state_, error_, info_, detail_));
    return true;
}

std::string LicenseService::StatusJson() const {
    std::string json;
    json.reserve(320);
    json += "{\"product\":\"";
    json += JsonEscape(kProductName);
    json += "\",\"state\":\"";
    json += LicenseStateName(state_);
    json += "\",\"edition\":\"";
    json += EditionName(EditionValue());          // 生效中的版次（Free 时就是 free）
    json += "\",\"licenseEdition\":\"";
    json += EditionName(info_.edition);           // 文件里写的版次：诊断用，两者不一致时一眼看出
    json += "\",\"holder\":\"";
    json += JsonEscape(info_.holder);
    json += "\",\"seats\":";
    json += std::to_string(info_.seats);
    json += ",\"expiresAt\":";
    if (info_.expiresAt.has_value()) {
        json += "\"";
        json += FormatIso8601Utc(*info_.expiresAt);
        json += "\"";
    } else {
        json += "null";                            // 没有到期时间 = 永久（或没有许可）
    }
    json += ",\"features\":[";
    const std::string names = FeatureListToString(features_);
    std::size_t start = 0;
    bool first = true;
    while (start < names.size()) {
        const std::size_t comma = names.find(',', start);
        const std::size_t stop = (comma == std::string::npos) ? names.size() : comma;
        if (!first) {
            json += ",";
        }
        json += "\"";
        json += names.substr(start, stop - start);
        json += "\"";
        first = false;
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    json += "]";
    json += ",\"trialDaysLeft\":0";   // 试用状态机是 B7：字段先占位，免得脚本两头改
    json += ",\"licenseFile\":\"";
    json += JsonEscape(NarrowPath(path_));
    json += "\",\"error\":\"";
    json += LicenseErrorName(error_);
    json += "\"}\n";
    return json;
}

std::string LicenseService::ExportChallenge(std::string_view appVersion, int seats,
                                            std::string_view holderUtf8) const {
    if (seats < 1) {
        seats = 1;
    }
    if (seats > 999) {
        seats = 999;   // 界面也校验，但这里兜底——申请串是合同的引子，数字不能荒唐
    }
    std::string holder(holderUtf8);
    for (char& ch : holder) {
        if (ch == '\r' || ch == '\n') {
            ch = ' ';   // 一行一个字段：换行会把申请串劈成两半
        }
    }
    std::string text;
    text.reserve(256);
    text += "product=";
    text += kProductName;
    text += "\nrequestId=";
    text += NewRequestId();
    text += "\nfingerprint=";
    text += Format(Current());
    text += "\nappVersion=";
    text += appVersion;
    text += "\nseats=";
    text += std::to_string(seats);
    text += "\nholder=";
    text += holder;
    text += "\n";
    return text;
}

}  // namespace npcc::license
