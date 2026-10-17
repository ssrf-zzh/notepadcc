#include "license/ActivationClient.h"

#include <windows.h>

#include <cstdint>
#include <vector>

#include "infra/Crypto.h"
#include "infra/Http.h"
#include "infra/TextCodec.h"
#include "license/LicenseService.h"   // JsonEscape

namespace npcc::license {
namespace {

// 响应体上限：许可文件几 KB，报错信息更小；64 KB 已经很宽松
constexpr std::size_t kMaxResponseBytes = 64u << 10;
constexpr int         kTimeoutMs = 10000;   // 设计 6.1：超时 10 秒

bool IsHexDigit(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

unsigned HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<unsigned>(ch - 'a' + 10);
    }
    return static_cast<unsigned>(ch - 'A' + 10);
}

unsigned Hex4(const std::string& text, std::size_t at, bool& ok) {
    ok = false;
    unsigned value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (at + i >= text.size() || !IsHexDigit(text[at + i])) {
            return 0;
        }
        value = value * 16u + HexValue(text[at + i]);
    }
    ok = true;
    return value;
}

void AppendUtf8(std::string& out, unsigned codePoint) {
    if (codePoint <= 0x7F) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// 解析一个 JSON 字符串字面量（调用方保证 text[at] == '"'）。
// 成功返回 true 并让 at 停在收尾引号之后。
bool ParseJsonString(const std::string& text, std::size_t& at, std::string& out) {
    if (at >= text.size() || text[at] != '"') {
        return false;
    }
    ++at;
    out.clear();
    while (at < text.size()) {
        const char ch = text[at];
        if (ch == '"') {
            ++at;
            return true;
        }
        if (ch != '\\') {
            out.push_back(ch);
            ++at;
            continue;
        }
        if (at + 1 >= text.size()) {
            return false;
        }
        const char esc = text[at + 1];
        at += 2;
        switch (esc) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                bool ok = false;
                const unsigned first = Hex4(text, at, ok);
                if (!ok) {
                    return false;
                }
                at += 4;
                unsigned codePoint = first;
                if (first >= 0xD800 && first <= 0xDBFF && at + 1 < text.size() &&
                    text[at] == '\\' && text[at + 1] == 'u') {
                    const unsigned second = Hex4(text, at + 2, ok);
                    if (ok && second >= 0xDC00 && second <= 0xDFFF) {
                        codePoint = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                        at += 6;
                    }
                }
                AppendUtf8(out, codePoint);
                break;
            }
            default:
                return false;   // 不认识的转义：宁可失败也不猜
        }
    }
    return false;   // 没有收尾引号
}

std::wstring HttpDetail(unsigned status) {
    return L"HTTP " + std::to_wstring(status);
}

}  // namespace

std::string ActivationErrorName(ActivationError error) {
    switch (error) {
        case ActivationError::None:                return "none";
        case ActivationError::NotConfigured:       return "notConfigured";
        case ActivationError::NetworkFailed:       return "networkFailed";
        case ActivationError::KeyNotFound:         return "keyNotFound";
        case ActivationError::KeyExhausted:        return "keyExhausted";
        case ActivationError::KeyDisabled:         return "keyDisabled";
        case ActivationError::FingerprintMismatch: return "fingerprintMismatch";
        case ActivationError::ServerError:         return "serverError";
        case ActivationError::BadResponse:         return "badResponse";
    }
    return "unknown";
}

std::wstring DescribeActivationError(ActivationError error) {
    switch (error) {
        case ActivationError::None:
            return L"激活成功";
        case ActivationError::NotConfigured:
            return L"还没有配置激活服务地址——请改用离线激活（把申请串发给客服）。";
        case ActivationError::NetworkFailed:
            return L"连不上激活服务——请检查网络与代理，或改用离线激活（把申请串发给客服）。";
        case ActivationError::KeyNotFound:
            return L"授权码不存在：请检查是否输错（去空格、统一大写后仍不认这个码）。";
        case ActivationError::KeyExhausted:
            return L"这张授权码的激活次数已用完：可以“停用当前机器”找回名额，或联系客服换绑。";
        case ActivationError::KeyDisabled:
            return L"该许可已被停用（退款 / 违规）：请联系客服。";
        case ActivationError::FingerprintMismatch:
            return L"该授权码已经绑定过别的机器：请联系客服走换绑流程。";
        case ActivationError::ServerError:
            return L"激活服务暂时出错——请稍后再试，或改用离线激活。";
        case ActivationError::BadResponse:
            return L"激活服务返回了看不懂的内容——请改用离线激活（把申请串发给客服）。";
    }
    return L"未知错误";
}

std::string NormalizeLicenseKey(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (char ch : key) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;   // 用户粘贴时常见的空格与换行：去掉，不算内容
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
        out.push_back(ch);
    }
    return out;
}

std::string MachineHashFromComputerName(std::string_view computerName) {
    const infra::Sha256Digest digest =
        infra::Sha256(computerName.data(), computerName.size());
    return digest.Hex().substr(0, 8);
}

std::string CurrentOsVersion() {
    // RtlGetVersion 不看清单的脸色（GetVersionEx 会按清单说谎）；从 ntdll 按需取，
    // 取不到就退回 "unknown"——激活请求里这一项缺失不影响协议。
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        if (fn != nullptr && fn(&info) == 0) {
            return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
                   "." + std::to_string(info.dwBuildNumber);
        }
    }
    return "unknown";
}

std::string BuildActivateRequestBody(const ActivateInput& input, const std::string& requestId,
                                     const std::string& machineHash) {
    std::string json = "{";
    json += "\"licenseKey\":\"" + JsonEscape(NormalizeLicenseKey(input.licenseKey)) + "\"";
    json += ",\"fingerprint\":\"" + JsonEscape(input.fingerprintText) + "\"";
    json += ",\"appVersion\":\"" + JsonEscape(input.appVersion) + "\"";
    json += ",\"osVersion\":\"" + JsonEscape(input.osVersion) + "\"";
    json += ",\"machineHash\":\"" + JsonEscape(machineHash) + "\"";
    json += ",\"requestId\":\"" + JsonEscape(requestId) + "\"";
    json += "}";
    return json;
}

bool JsonStringField(const std::string& json, std::string_view key, std::string& value) {
    std::size_t at = 0;
    while (at < json.size()) {
        if (json[at] != '"') {
            ++at;
            continue;
        }
        std::string token;
        if (!ParseJsonString(json, at, token)) {
            return false;   // 字符串本身坏掉了：后面没法再扫
        }
        // 键之后必须是 ':'，值必须是字符串——协议里只有平铺的字符串字段
        std::size_t probe = at;
        while (probe < json.size() && (json[probe] == ' ' || json[probe] == '\t' ||
                                       json[probe] == '\r' || json[probe] == '\n')) {
            ++probe;
        }
        if (probe >= json.size() || json[probe] != ':') {
            continue;   // 这是某个值，不是键
        }
        ++probe;
        while (probe < json.size() && (json[probe] == ' ' || json[probe] == '\t' ||
                                       json[probe] == '\r' || json[probe] == '\n')) {
            ++probe;
        }
        if (probe >= json.size() || json[probe] != '"') {
            continue;   // 值不是字符串（数字 / 布尔 / null / 对象）：跳过
        }
        std::string text;
        if (!ParseJsonString(json, probe, text)) {
            return false;
        }
        if (token == key) {
            value = std::move(text);
            return true;
        }
        at = probe;
    }
    return false;
}

ActivationResult ParseActivateResponse(unsigned status, const std::string& body) {
    ActivationResult result;
    if (status == 200) {
        std::string encoded;
        if (!JsonStringField(body, "license", encoded) || encoded.empty()) {
            result.error = ActivationError::BadResponse;
            result.detail = L"200 响应里没有 license 字段";
            return result;
        }
        if (!infra::Base64Decode(encoded, result.licenseBytes)) {
            result.error = ActivationError::BadResponse;
            result.licenseBytes.clear();
            result.detail = L"license 字段不是合法的 Base64";
            return result;
        }
        std::string message;
        if (JsonStringField(body, "message", message)) {
            result.message = infra::WideFromUtf8(message);
        }
        return result;
    }
    if (status >= 500) {
        result.error = ActivationError::ServerError;
        result.detail = HttpDetail(status);
        return result;
    }
    if (status >= 400) {
        std::string code;
        JsonStringField(body, "error", code);
        if (code == "KEY_NOT_FOUND") {
            result.error = ActivationError::KeyNotFound;
        } else if (code == "KEY_EXHAUSTED") {
            result.error = ActivationError::KeyExhausted;
        } else if (code == "KEY_DISABLED") {
            result.error = ActivationError::KeyDisabled;
        } else if (code == "FINGERPRINT_MISMATCH") {
            result.error = ActivationError::FingerprintMismatch;
        } else if (code == "SERVER_ERROR") {
            result.error = ActivationError::ServerError;
        } else {
            result.error = ActivationError::BadResponse;
            result.detail = code.empty()
                                ? HttpDetail(status) + L"：响应体里没有 error 字段"
                                : L"不认识的错误码：" + infra::WideFromUtf8(code);
            return result;
        }
        std::string message;
        if (JsonStringField(body, "message", message)) {
            result.message = infra::WideFromUtf8(message);
        }
        result.detail = HttpDetail(status);
        return result;
    }
    result.error = ActivationError::BadResponse;
    result.detail = L"意外的 HTTP 状态码 " + std::to_wstring(status);
    return result;
}

ActivationClient::ActivationClient(std::string serviceUrl) : serviceUrl_(std::move(serviceUrl)) {}

ActivationResult ActivationClient::Activate(const ActivateInput& input) const {
    ActivationResult result;
    std::string base = serviceUrl_;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.empty()) {
        result.error = ActivationError::NotConfigured;
        result.detail = L"没有配置激活服务地址";
        return result;
    }

    const std::string url = base + "/v1/activate";
    const std::string machineHash = MachineHashFromComputerName(input.computerName);
    const std::string requestId = NewRequestId();
    const std::string body = BuildActivateRequestBody(input, requestId, machineHash);

    // 设计 6.1：超时 10 秒、只重试一次。重试只针对网络层失败——4xx/5xx 是服务端
    // 已经给出答复，重发只会放大流量。
    infra::HttpResponse response = infra::HttpPostJson(url, body, kMaxResponseBytes, kTimeoutMs);
    if (!response.ok) {
        response = infra::HttpPostJson(url, body, kMaxResponseBytes, kTimeoutMs);
    }
    if (!response.ok) {
        result.error = ActivationError::NetworkFailed;
        result.detail = response.error;
        return result;
    }
    return ParseActivateResponse(response.statusCode,
                                 std::string(response.bytes.begin(), response.bytes.end()));
}

}  // namespace npcc::license
