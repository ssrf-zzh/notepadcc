#include "core/UpdateManifest.h"

#include <cstdint>
#include <vector>

#include "core/JsonFlat.h"

namespace npcc::core {
namespace {

bool AllDigits(std::string_view text) {
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

// "1.0.1" → {1, 0, 1}。空段（"1." / "1..0"）与超范围（> INT32_MAX）都算非法。
bool ParseVersionSegments(std::string_view text, std::vector<std::uint64_t>& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = text.find('.', start);
        const std::string_view segment =
            text.substr(start, dot == std::string_view::npos ? std::string_view::npos
                                                             : dot - start);
        if (!AllDigits(segment)) {
            return false;
        }
        std::uint64_t value = 0;
        for (const char ch : segment) {
            value = value * 10 + static_cast<std::uint64_t>(ch - '0');
            if (value > 0x7FFFFFFFull) {
                return false;
            }
        }
        out.push_back(value);
        if (dot == std::string_view::npos) {
            return true;
        }
        start = dot + 1;
    }
}

bool RequireStringField(const JsonObject& object, std::string_view key, std::string& out,
                        std::wstring* error) {
    const auto found = object.values.find(std::string(key));
    if (found == object.values.end()) {
        if (error != nullptr) {
            *error = L"清单缺少字段 \"" + std::wstring(key.begin(), key.end()) + L"\"";
        }
        return false;
    }
    if (found->second.kind != JsonValue::Kind::String) {
        if (error != nullptr) {
            *error = L"清单字段 \"" + std::wstring(key.begin(), key.end()) + L"\" 不是字符串";
        }
        return false;
    }
    out = found->second.text;
    return true;
}

}  // namespace

bool IsValidSha256Hex(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    for (const char ch : text) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                         (ch >= 'A' && ch <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

std::string LowerHexAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'F') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

int CompareVersions(std::string_view a, std::string_view b, bool* valid) {
    std::vector<std::uint64_t> left;
    std::vector<std::uint64_t> right;
    if (!ParseVersionSegments(a, left) || !ParseVersionSegments(b, right)) {
        if (valid != nullptr) {
            *valid = false;
        }
        return 0;
    }
    if (valid != nullptr) {
        *valid = true;
    }
    const std::size_t count = left.size() > right.size() ? left.size() : right.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t l = i < left.size() ? left[i] : 0;
        const std::uint64_t r = i < right.size() ? right[i] : 0;
        if (l < r) {
            return -1;
        }
        if (l > r) {
            return 1;
        }
    }
    return 0;
}

bool IsNewerVersion(std::string_view candidate, std::string_view current, bool* valid) {
    bool ok = false;
    const int compared = CompareVersions(candidate, current, &ok);
    if (valid != nullptr) {
        *valid = ok;
    }
    return ok && compared > 0;
}

bool IsAllowedUpdateUrl(std::string_view url, std::wstring* error) {
    std::string lower(url);
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    if (lower.rfind("https://", 0) == 0) {
        return true;
    }
    if (lower.rfind("http://", 0) == 0) {
        const std::string_view rest(lower.data() + 7, lower.size() - 7);
        const std::size_t end = rest.find_first_of("/:");
        const std::string_view host = rest.substr(0, end);
        if (host == "127.0.0.1" || host == "localhost") {
            return true;
        }
        if (error != nullptr) {
            *error = L"下载源只允许 https（明文 http 只对本机 127.0.0.1/localhost 放行，"
                     L"供回归与本地测试用）";
        }
        return false;
    }
    if (error != nullptr) {
        *error = L"下载源必须以 https:// 开头";
    }
    return false;
}

bool ParseUpdateManifest(std::string_view utf8, UpdateManifest& out, std::wstring* error) {
    JsonObject object;
    std::wstring parseError;
    if (!ParseJsonObject(utf8, object, &parseError)) {
        if (error != nullptr) {
            *error = L"清单不是合法 JSON：" + parseError;
        }
        return false;
    }
    if (object.values.size() != 4) {
        if (error != nullptr) {
            *error = L"清单应当正好有 version / notes / url / sha256 四个字段（实际 " +
                     std::to_wstring(object.values.size()) + L" 个）";
        }
        return false;
    }
    UpdateManifest parsed;
    if (!RequireStringField(object, "version", parsed.version, error) ||
        !RequireStringField(object, "notes", parsed.notes, error) ||
        !RequireStringField(object, "url", parsed.url, error) ||
        !RequireStringField(object, "sha256", parsed.sha256, error)) {
        return false;
    }
    bool versionOk = false;
    CompareVersions(parsed.version, parsed.version, &versionOk);
    if (!versionOk) {
        if (error != nullptr) {
            *error = L"清单里的版本号 \"" + std::wstring(parsed.version.begin(), parsed.version.end()) +
                     L"\" 不是数字点分格式";
        }
        return false;
    }
    if (!IsValidSha256Hex(parsed.sha256)) {
        if (error != nullptr) {
            *error = L"清单里的 sha256 不是 64 位十六进制（实际 " +
                     std::to_wstring(parsed.sha256.size()) + L" 字符）";
        }
        return false;
    }
    if (parsed.url.empty()) {
        if (error != nullptr) {
            *error = L"清单里的 url 是空的";
        }
        return false;
    }
    parsed.sha256 = LowerHexAscii(parsed.sha256);
    out = std::move(parsed);
    return true;
}

}  // namespace npcc::core
