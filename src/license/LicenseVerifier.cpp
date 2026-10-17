#include "license/LicenseVerifier.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <vector>

#include "infra/Crypto.h"
#include "infra/TextCodec.h"
#include "infra/Xml.h"
#include "license/LicenseKeys.h"

namespace npcc::license {
namespace {

constexpr std::size_t kMaxContainerBytes = 256u << 10;   // 容器是几 KB 的东西，256 KB 足够
constexpr std::size_t kMaxPayloadBytes = 64u << 10;
constexpr std::size_t kMaxHolderBytes = 256;
constexpr std::size_t kMaxFieldBytes = 512;
constexpr int kSupportedSchema = 1;

using infra::Utf8FromWide;
using infra::WideFromUtf8;

bool IsAsciiSpace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\v' || ch == L'\f' || ch == L'\r' || ch == L'\n';
}

std::wstring TrimAscii(std::wstring_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsAsciiSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && IsAsciiSpace(text[end - 1])) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

bool IsAllDigits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

bool IsHexDigit(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool IsValidUuid(std::string_view text) {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') {
                return false;
            }
        } else if (!IsHexDigit(text[index])) {
            return false;
        }
    }
    return true;
}

bool IsKnownPayloadKey(std::string_view key) {
    static constexpr std::string_view kKeys[] = {
        "schema",  "product",   "licenseId",       "edition", "seats",  "holder",
        "holderId", "issuedAt", "notBefore",       "expiresAt", "updateUntil", "features",
        "restrictions", "fingerprintMode", "fingerprint", "channel",
    };
    return std::find(std::begin(kKeys), std::end(kKeys), key) != std::end(kKeys);
}

LicenseVerifyResult Fail(LicenseError error, std::wstring detail) {
    LicenseVerifyResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

// payload 的 key=value 行 → LicenseInfo。任何结构问题都算 MalformedContainer：
// payload 虽已验签（是签发方写的），但签发方写错同样是"格式不正确"，
// 分开报只会让用户多猜一层。
LicenseError ParsePayloadFields(std::string_view bytes, LicenseInfo* info, std::wstring* detail) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        *detail = L"payload 不该带 UTF-8 BOM";
        return LicenseError::MalformedContainer;
    }

    std::map<std::string, std::string, std::less<>> fields;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        std::size_t end = bytes.find('\n', begin);
        if (end == std::string_view::npos) {
            end = bytes.size();
        }
        std::string_view line = bytes.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);   // 容忍 CRLF（签发端约定 LF，宽容读取不削弱任何东西）
        }
        begin = end + 1;   // 越界也没关系：下一轮 begin < size 不成立就收尾
        if (line.empty()) {
            *detail = L"payload 里有空行";
            return LicenseError::MalformedContainer;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            *detail = L"payload 行不是 key=value 形式：" + WideFromUtf8(line);
            return LicenseError::MalformedContainer;
        }
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        for (const char ch : key) {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                            (ch >= '0' && ch <= '9') || ch == '_';
            if (!ok) {
                *detail = L"payload 键名含非法字符：" + WideFromUtf8(key);
                return LicenseError::MalformedContainer;
            }
        }
        if (!IsKnownPayloadKey(key)) {
            *detail = L"payload 里有不认识的键：" + WideFromUtf8(key);
            return LicenseError::MalformedContainer;
        }
        if (value.size() > kMaxFieldBytes) {
            *detail = L"payload 字段过长：" + WideFromUtf8(key);
            return LicenseError::MalformedContainer;
        }
        if (!fields.emplace(std::string(key), std::string(value)).second) {
            *detail = L"payload 键重复出现：" + WideFromUtf8(key);
            return LicenseError::MalformedContainer;
        }
    }

    const auto require = [&](std::string_view key) -> const std::string* {
        const auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    };
    for (const std::string_view key :
         {"schema", "product", "licenseId", "edition", "seats", "holder", "issuedAt", "features"}) {
        if (fields.find(key) == fields.end()) {
            *detail = L"payload 缺必填键：" + WideFromUtf8(key);
            return LicenseError::MalformedContainer;
        }
    }

    // schema：比本版本新 → 明确的"请升级"错误；不是数字 / 是 0 → 格式错误
    const std::string& schemaText = *require("schema");
    if (!IsAllDigits(schemaText)) {
        *detail = L"schema 不是十进制数字：" + WideFromUtf8(schemaText);
        return LicenseError::MalformedContainer;
    }
    const int schema = std::atoi(schemaText.c_str());
    if (schema > kSupportedSchema) {
        *detail = L"schema=" + WideFromUtf8(schemaText) + L"，本版本只支持 " +
                  std::to_wstring(kSupportedSchema);
        return LicenseError::SchemaTooNew;
    }
    if (schema < 1) {
        *detail = L"schema 不能是 0";
        return LicenseError::MalformedContainer;
    }
    info->schema = schema;

    const std::string& product = *require("product");
    if (product != kProductName) {
        *detail = L"product=" + WideFromUtf8(product) + L"，不是本产品（" +
                  WideFromUtf8(kProductName) + L"）";
        return LicenseError::ProductMismatch;
    }
    info->product = product;

    const std::string& licenseId = *require("licenseId");
    if (!IsValidUuid(licenseId)) {
        *detail = L"licenseId 不是 UUID：" + WideFromUtf8(licenseId);
        return LicenseError::MalformedContainer;
    }
    info->licenseId = licenseId;

    const std::string& editionText = *require("edition");
    const std::optional<Edition> edition = EditionFromName(editionText);
    if (!edition.has_value()) {
        *detail = L"edition 取值不认识：" + WideFromUtf8(editionText);
        return LicenseError::MalformedContainer;
    }
    info->edition = *edition;

    const std::string& seatsText = *require("seats");
    if (!IsAllDigits(seatsText) || seatsText.size() > 9) {
        *detail = L"seats 必须是不超过 9 位的非负整数：" + WideFromUtf8(seatsText);
        return LicenseError::MalformedContainer;
    }
    info->seats = static_cast<unsigned int>(std::strtoul(seatsText.c_str(), nullptr, 10));

    const std::string& holder = *require("holder");
    if (holder.empty() || holder.size() > kMaxHolderBytes) {
        *detail = L"holder 不能为空且不超过 256 字节";
        return LicenseError::MalformedContainer;
    }
    info->holder = holder;

    if (const std::string* holderId = require("holderId"); holderId != nullptr) {
        info->holderId = *holderId;
    }

    const auto parseTime = [&](std::string_view key, std::optional<std::int64_t>* out) -> bool {
        const std::string* text = require(key);
        if (text == nullptr) {
            return true;   // 可选字段
        }
        const std::optional<std::int64_t> value = ParseIso8601Utc(*text);
        if (!value.has_value()) {
            *detail = WideFromUtf8(key) + L" 不是 ISO8601 UTC（YYYY-MM-DDThh:mm:ssZ）：" +
                      WideFromUtf8(*text);
            return false;
        }
        *out = *value;
        return true;
    };
    if (!parseTime("issuedAt", &info->issuedAt) || !parseTime("notBefore", &info->notBefore) ||
        !parseTime("expiresAt", &info->expiresAt) || !parseTime("updateUntil", &info->updateUntil)) {
        return LicenseError::MalformedContainer;
    }
    if (info->issuedAt.has_value() && info->expiresAt.has_value() &&
        *info->expiresAt < *info->issuedAt) {
        *detail = L"expiresAt 早于 issuedAt，时间窗不成立";
        return LicenseError::MalformedContainer;
    }

    // features：逗号分隔；不认识的名字**不授权、但也不作废整张许可**——
    // 老版本收到新版本签发的许可时，认识的部分照用，不认识的名字列出来。
    const std::string& featuresText = *require("features");
    info->features = 0;
    info->unknownFeatures.clear();
    if (!featuresText.empty()) {
        std::size_t pos = 0;
        while (pos <= featuresText.size()) {
            const std::size_t comma = featuresText.find(',', pos);
            const std::string token =
                featuresText.substr(pos, comma == std::string::npos ? std::string::npos
                                                                     : comma - pos);
            pos = comma == std::string::npos ? featuresText.size() + 1 : comma + 1;
            if (token.empty()) {
                *detail = L"features 里有空项（多余的逗号？）";
                return LicenseError::MalformedContainer;
            }
            const std::optional<Feature> feature = FeatureFromName(token);
            if (feature.has_value()) {
                info->features |= FeatureBit(*feature);
            } else {
                info->unknownFeatures.push_back(token);
            }
        }
    }

    if (const std::string* restrictions = require("restrictions"); restrictions != nullptr) {
        info->restrictions = *restrictions;
    }

    info->fingerprintMode = FingerprintMode::Tolerant;
    if (const std::string* modeText = require("fingerprintMode"); modeText != nullptr) {
        const std::optional<FingerprintMode> mode = FingerprintModeFromName(*modeText);
        if (!mode.has_value()) {
            *detail = L"fingerprintMode 取值不认识：" + WideFromUtf8(*modeText);
            return LicenseError::MalformedContainer;
        }
        info->fingerprintMode = *mode;
    }

    if (const std::string* fingerprint = require("fingerprint"); fingerprint != nullptr) {
        info->fingerprint = *fingerprint;
    }
    if (info->fingerprintMode == FingerprintMode::None) {
        if (!info->fingerprint.empty()) {
            *detail = L"fingerprintMode=none 时 fingerprint 必须留空（站点许可不绑机器）";
            return LicenseError::MalformedContainer;
        }
    } else if (info->fingerprint.empty()) {
        *detail = L"fingerprintMode=" + WideFromUtf8(FingerprintModeName(info->fingerprintMode)) +
                  L" 时必须给 fingerprint";
        return LicenseError::MalformedContainer;
    }

    if (const std::string* channel = require("channel"); channel != nullptr) {
        if (!channel->empty() && *channel != "stable" && *channel != "beta") {
            *detail = L"channel 取值不认识：" + WideFromUtf8(*channel);
            return LicenseError::MalformedContainer;
        }
        info->channel = *channel;
    }
    return LicenseError::None;
}

}  // namespace

LicenseVerifyResult VerifyLicenseBytes(std::string_view containerXml, std::int64_t nowUnix) {
    if (containerXml.size() > kMaxContainerBytes) {
        return Fail(LicenseError::MalformedContainer, L"许可文件超过 256 KB，不像是许可容器");
    }

    infra::XmlDocument document;
    std::wstring xmlError;
    if (!infra::XmlDocument::Parse(containerXml, document, &xmlError)) {
        return Fail(LicenseError::MalformedContainer, L"XML 解析失败：" + xmlError);
    }
    const infra::XmlElement* root = document.Root();
    if (root == nullptr || root->name != L"NotepadccLicense") {
        return Fail(LicenseError::MalformedContainer, L"根元素不是 NotepadccLicense");
    }
    for (const infra::XmlElementPtr& child : root->children) {
        if (child->name != L"Payload" && child->name != L"Signature") {
            return Fail(LicenseError::MalformedContainer,
                        L"容器里有不认识的元素：<" + child->name + L">");
        }
    }
    const infra::XmlElement* payload = root->Child(L"Payload");
    const infra::XmlElement* signature = root->Child(L"Signature");
    if (payload == nullptr || signature == nullptr ||
        root->Children(L"Payload").size() != 1 || root->Children(L"Signature").size() != 1) {
        return Fail(LicenseError::MalformedContainer,
                    L"容器必须正好有一个 <Payload> 与一个 <Signature>");
    }

    const std::wstring* encoding = payload->Attribute(L"encoding");
    if (encoding == nullptr || *encoding != L"base64") {
        return Fail(LicenseError::MalformedContainer, L"<Payload> 的 encoding 必须是 base64");
    }
    const std::wstring payloadText = TrimAscii(payload->text);
    if (payloadText.empty()) {
        return Fail(LicenseError::MalformedContainer, L"<Payload> 是空的（没有被签名的内容）");
    }
    std::string payloadBytes;
    if (!infra::Base64Decode(Utf8FromWide(payloadText), payloadBytes) || payloadBytes.empty()) {
        return Fail(LicenseError::MalformedContainer, L"<Payload> 不是合法的 base64");
    }
    if (payloadBytes.size() > kMaxPayloadBytes) {
        return Fail(LicenseError::MalformedContainer, L"payload 超过 64 KB，不像是许可内容");
    }

    const std::wstring* alg = signature->Attribute(L"alg");
    if (alg == nullptr || *alg != WideFromUtf8(kLicenseAlgName)) {
        return Fail(LicenseError::MalformedContainer,
                    L"签名算法不认识（alg 应为 RSA-2048-PKCS1-SHA256）");
    }
    const std::wstring* keyId = signature->Attribute(L"keyId");
    if (keyId == nullptr || keyId->empty()) {
        return Fail(LicenseError::MalformedContainer, L"<Signature> 缺 keyId");
    }
    const std::string keyIdUtf8 = Utf8FromWide(*keyId);
    const std::string_view publicKey = FindLicensePublicKey(keyIdUtf8);
    if (publicKey.empty()) {
        return Fail(LicenseError::KeyUnknown,
                    L"签发密钥 keyId=" + *keyId + L" 不在本版本内置的公钥表里");
    }
    const std::wstring signatureText = TrimAscii(signature->text);
    if (signatureText.empty()) {
        return Fail(LicenseError::MalformedContainer, L"<Signature> 是空的");
    }
    std::wstring why;
    if (!infra::RsaVerifyPkcs1Sha256(payloadBytes.data(), payloadBytes.size(),
                                     Utf8FromWide(signatureText), publicKey, &why)) {
        return Fail(LicenseError::SignatureInvalid,
                    why.empty() ? L"签名不匹配" : why + L"（请确认文件未被修改）");
    }

    LicenseVerifyResult result;
    result.info.keyId = keyIdUtf8;
    result.error = ParsePayloadFields(payloadBytes, &result.info, &result.detail);
    if (result.error != LicenseError::None) {
        return result;
    }

    if (result.info.notBefore.has_value() && nowUnix < *result.info.notBefore) {
        result.error = LicenseError::NotYetValid;
        result.detail = L"notBefore=" + WideFromUtf8(FormatIso8601Utc(*result.info.notBefore)) +
                        L"，当前时间还没到";
        return result;
    }
    if (result.info.expiresAt.has_value() && nowUnix >= *result.info.expiresAt) {
        result.error = LicenseError::Expired;
        result.detail = L"expiresAt=" + WideFromUtf8(FormatIso8601Utc(*result.info.expiresAt)) +
                        L"，许可已过期";
        return result;
    }
    return result;
}

std::string SerializeLicensePayload(const LicenseInfo& info) {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!info.channel.empty()) {
        pairs.emplace_back("channel", info.channel);
    }
    pairs.emplace_back("edition", std::string(EditionName(info.edition)));
    if (info.expiresAt.has_value()) {
        pairs.emplace_back("expiresAt", FormatIso8601Utc(*info.expiresAt));
    }
    pairs.emplace_back("features", FeatureListToString(info.features));
    if (!info.fingerprint.empty()) {
        pairs.emplace_back("fingerprint", info.fingerprint);
    }
    pairs.emplace_back("fingerprintMode", std::string(FingerprintModeName(info.fingerprintMode)));
    pairs.emplace_back("holder", info.holder);
    if (!info.holderId.empty()) {
        pairs.emplace_back("holderId", info.holderId);
    }
    if (info.issuedAt.has_value()) {
        pairs.emplace_back("issuedAt", FormatIso8601Utc(*info.issuedAt));
    }
    pairs.emplace_back("licenseId", info.licenseId);
    if (info.notBefore.has_value()) {
        pairs.emplace_back("notBefore", FormatIso8601Utc(*info.notBefore));
    }
    pairs.emplace_back("product", info.product.empty() ? std::string(kProductName) : info.product);
    if (!info.restrictions.empty()) {
        pairs.emplace_back("restrictions", info.restrictions);
    }
    pairs.emplace_back("schema", std::to_string(info.schema));
    pairs.emplace_back("seats", std::to_string(info.seats));
    if (info.updateUntil.has_value()) {
        pairs.emplace_back("updateUntil", FormatIso8601Utc(*info.updateUntil));
    }

    std::string out;
    for (const auto& [key, value] : pairs) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        out.append(key);
        out.push_back('=');
        out.append(value);
    }
    return out;
}

}  // namespace npcc::license
