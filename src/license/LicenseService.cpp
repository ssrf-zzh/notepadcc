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
                        const std::wstring& detail, int trialDaysLeft, int graceDaysLeft,
                        bool clockAnomaly) {
    if (state == LicenseState::Trial) {
        std::wstring line = L"许可：试用中（还剩 " + std::to_wstring(trialDaysLeft) + L" 天）";
        if (clockAnomaly) {
            line += L"，检测到时钟回拨（按保守值计算）";
        }
        return line;
    }
    if (state == LicenseState::Grace) {
        std::wstring line = L"许可：宽限期（已到期，还剩 " + std::to_wstring(graceDaysLeft) +
                            L" 天，功能仍可用）";
        if (!info.holder.empty()) {
            line += L"，持有人「" + infra::WideFromUtf8(info.holder) + L"」";
        }
        if (info.expiresAt.has_value()) {
            line += L"，到期 " + infra::WideFromUtf8(FormatIso8601Utc(*info.expiresAt));
        }
        return line;
    }
    if (state != LicenseState::Free) {
        std::wstring line = L"许可：已激活";
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

TrialEvaluation EvaluateTrial(const TrialRecord& record, std::int64_t now) {
    TrialEvaluation evaluation;
    if (!record.firstRunUtc.has_value()) {
        return evaluation;   // 试用还没开始（第一次运行的记录还没写下）
    }
    evaluation.started = true;
    if (record.lastSeenUtc.has_value() && now < *record.lastSeenUtc - kClockBackstepSeconds) {
        evaluation.clockAnomaly = true;   // 回拨超过 24 小时（设计 5.2）
    }
    std::int64_t effectiveNow = now;
    if (record.lastSeenUtc.has_value() && *record.lastSeenUtc > effectiveNow) {
        effectiveNow = *record.lastSeenUtc;   // 保守值：回拨不白得天数
    }
    const std::int64_t expiry = *record.firstRunUtc + kTrialDays * 86400;
    const std::int64_t remaining = expiry - effectiveNow;
    if (remaining > 0) {
        evaluation.active = true;
        evaluation.daysLeft = static_cast<int>((remaining + 86399) / 86400);
        if (evaluation.daysLeft > static_cast<int>(kTrialDays)) {
            evaluation.daysLeft = static_cast<int>(kTrialDays);   // 防御：记录被改得离谱
        }
    }
    return evaluation;
}

int RemainingGraceDays(const LicenseInfo& info, std::int64_t now) {
    if (!info.expiresAt.has_value() || now < *info.expiresAt) {
        return 0;
    }
    const std::int64_t graceEnd = *info.expiresAt + kGraceDays * 86400;
    if (now >= graceEnd) {
        return 0;
    }
    return static_cast<int>((graceEnd - now + 86399) / 86400);
}

SessionEvaluation EvaluateSession(const LicenseVerifyResult& verified, const Fingerprint& machine,
                                  const TrialRecord& record, std::int64_t now) {
    SessionEvaluation evaluation;
    const TrialEvaluation trial = EvaluateTrial(record, now);
    evaluation.clockAnomaly = trial.clockAnomaly;
    evaluation.trialActive = trial.active;

    if (verified.ok()) {
        evaluation.fingerprint =
            Match(machine, verified.info.fingerprint, verified.info.fingerprintMode);
        if (evaluation.fingerprint.Allows()) {
            if (!verified.info.expiresAt.has_value() || *verified.info.expiresAt > now) {
                evaluation.state = LicenseState::Licensed;   // 有效期内（或永久）
                evaluation.features = verified.info.features;
                return evaluation;
            }
            const int grace = RemainingGraceDays(verified.info, now);
            if (grace > 0) {
                // 宽限期（设计 5.3）：功能仍可用，提示续期
                evaluation.state = LicenseState::Grace;
                evaluation.error = LicenseError::Expired;
                evaluation.features = verified.info.features;
                evaluation.graceDaysLeft = grace;
                return evaluation;
            }
            evaluation.error = LicenseError::Expired;
        } else {
            evaluation.error = LicenseError::FingerprintMismatch;
        }
    } else {
        evaluation.error = verified.error;
    }

    if (trial.active) {
        // 没有可用许可但试用还在窗口内：按 Pro 全集放行（设计 2.2）
        evaluation.state = LicenseState::Trial;
        evaluation.features = ProFeatureMask();
        evaluation.trialDaysLeft = trial.daysLeft;
        if (trial.clockAnomaly) {
            // 设计 9 的错误表：时钟异常记 Warn；错误名进 --license-info 的 JSON
            evaluation.error = LicenseError::ClockAnomaly;
        }
    }
    return evaluation;
}

StartupNotice SelectStartupNotice(const LicenseInfo& info, LicenseState state,
                                  const TrialRecord& record, bool trialActive, int graceDaysLeft,
                                  bool trialShown, bool graceShown, bool clockShown,
                                  bool clockAnomaly) {
    StartupNotice notice;
    if (state == LicenseState::Grace && graceDaysLeft > 0 && !record.graceNoticeDismissed &&
        !graceShown) {
        notice.kind = LicenseNotice::Grace;
        notice.daysLeft = graceDaysLeft;
        notice.holder = info.holder;
        notice.expiresAt = info.expiresAt;
        return notice;
    }
    if (state == LicenseState::Free && record.firstRunUtc.has_value() && !trialActive &&
        !record.trialNoticeDismissed && !trialShown) {
        notice.kind = LicenseNotice::TrialExpired;   // 试用到期回落（设计 5.3）
        return notice;
    }
    if (state == LicenseState::Trial && clockAnomaly && !clockShown) {
        notice.kind = LicenseNotice::ClockAnomaly;   // 只在试用状态露面（设计 5.2）
        return notice;
    }
    return notice;
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
    trialActive_ = false;
    trialDaysLeft_ = 0;
    graceDaysLeft_ = 0;
    clockAnomaly_ = false;

    // 文件读不出来也是一条判定输入（FileMissing 是 Free 的正常状态）：照样走
    // 会话判定——试用窗口该兜的底还得兜。
    LicenseVerifyResult verified;
    std::string bytes;
    LicenseError loadError = LicenseError::None;
    std::wstring loadDetail;
    if (LoadLicenseFile(path_, bytes, &loadError, &loadDetail)) {
        verified = VerifyLicenseBytes(bytes, checkedAt_);
    } else {
        verified.error = loadError;
        verified.detail = std::move(loadDetail);
    }

    const SessionEvaluation evaluation = EvaluateSession(verified, Current(), record_, checkedAt_);
    fingerprint_ = evaluation.fingerprint;
    error_ = evaluation.error;
    detail_ = std::move(verified.detail);
    info_ = verified.info;   // 过期 / 未生效 / 指纹不符也留着内容：界面要说清"谁的许可"
    state_ = evaluation.state;
    features_ = evaluation.features;
    trialActive_ = evaluation.trialActive;
    trialDaysLeft_ = evaluation.trialDaysLeft;
    graceDaysLeft_ = evaluation.graceDaysLeft;
    clockAnomaly_ = evaluation.clockAnomaly;
    LogStatus();
    return state_ != LicenseState::Free;
}

void LicenseService::LogStatus() const {
    const std::wstring line = StatusLine(state_, error_, info_, detail_, trialDaysLeft_,
                                         graceDaysLeft_, clockAnomaly_);
    if (LicenseErrorLevel(error_) == LicenseLogLevel::Warn) {
        infra::LogWarn(L"license", line);
    } else {
        infra::LogInfo(L"license", line);
    }
}

Edition LicenseService::EditionValue() const {
    if (state_ == LicenseState::Trial) {
        return Edition::Pro;   // 试用期视为 Pro（设计 2.1）
    }
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
    report(LicenseError::None,
           StatusLine(state_, error_, info_, detail_, trialDaysLeft_, graceDaysLeft_,
                      clockAnomaly_));
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
    json += ",\"trialDaysLeft\":";
    json += std::to_string(trialDaysLeft_);   // 试用剩余天数（B7）
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

void LicenseService::BeginSession(const TrialEnvironment* environment) {
    statePath_ = (environment != nullptr && !environment->stateFile.empty())
                     ? environment->stateFile
                     : DefaultTrialStatePath();
    registrySubKey_ = (environment != nullptr)
                          ? environment->registrySubKey
                          : std::wstring(L"Software\\notepadcc");

    const std::int64_t now = NowUnixUtc();
    TrialRecord record;
    std::wstring detail;
    if (!LoadTrialState(statePath_, record, &detail)) {
        infra::LogWarn(L"license", L"试用记录读不出来，按空记录处理 — " + detail);
        record = TrialRecord{};
    }
    std::optional<std::int64_t> registryFirst;
    if (!registrySubKey_.empty()) {
        registryFirst = ReadRegistryFirstRun(registrySubKey_);
    }
    record = MergeTrialRecords(record, registryFirst);
    const bool firstRun = !record.firstRunUtc.has_value();
    if (firstRun) {
        record.firstRunUtc = now;   // 首次运行（双写从这里开始）
    }
    if (!record.lastSeenUtc.has_value() || *record.lastSeenUtc < now) {
        record.lastSeenUtc = now;   // 每次启动都记一笔（时钟回拨判定用）
    }
    record_ = record;
    sessionStarted_ = true;

    std::wstring saveDetail;
    if (!SaveTrialState(statePath_, record_, &saveDetail)) {
        infra::LogWarn(L"license", L"试用记录写不进去（不影响使用）— " + saveDetail);
    }
    if (!registrySubKey_.empty() &&
        (!registryFirst.has_value() || *record_.firstRunUtc < *registryFirst)) {
        if (!WriteRegistryFirstRun(*record_.firstRunUtc, registrySubKey_)) {
            infra::LogWarn(L"license", L"首次运行时间写不进注册表（不影响使用）");
        }
    }
    std::wstring line = firstRun ? L"许可会话：首次运行，试用从这次开始"
                                 : L"许可会话：续记上次运行时间";
    line += L"（first=" + std::to_wstring(*record_.firstRunUtc) + L"）";
    infra::LogInfo(L"license", line);
    Reload();
}

StartupNotice LicenseService::PendingStartupNotice() const {
    return SelectStartupNotice(info_, state_, record_, trialActive_, graceDaysLeft_,
                               noticeTrialShown_, noticeGraceShown_, noticeClockShown_,
                               clockAnomaly_);
}

void LicenseService::MarkNoticeShown(LicenseNotice kind) {
    switch (kind) {
        case LicenseNotice::TrialExpired: noticeTrialShown_ = true; break;
        case LicenseNotice::Grace: noticeGraceShown_ = true; break;
        case LicenseNotice::ClockAnomaly: noticeClockShown_ = true; break;
        case LicenseNotice::None: break;
    }
}

bool LicenseService::DismissNoticeForever(LicenseNotice kind) {
    bool changed = false;
    if (kind == LicenseNotice::TrialExpired && !record_.trialNoticeDismissed) {
        record_.trialNoticeDismissed = true;
        changed = true;
    }
    if (kind == LicenseNotice::Grace && !record_.graceNoticeDismissed) {
        record_.graceNoticeDismissed = true;
        changed = true;
    }
    if (!changed) {
        return true;   // 时钟异常没有"不再提示"；没有要写的也当成功
    }
    if (!sessionStarted_) {
        return false;   // 还没 BeginSession：没有落点
    }
    std::wstring detail;
    if (!SaveTrialState(statePath_, record_, &detail)) {
        infra::LogWarn(L"license", L"「不再提示」写不进去 — " + detail);
        return false;
    }
    return true;
}

}  // namespace npcc::license
