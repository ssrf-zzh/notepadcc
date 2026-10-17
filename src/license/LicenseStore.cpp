#include "license/LicenseStore.h"

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <string>
#include <system_error>
#include <utility>

#include "infra/Paths.h"
#include "infra/Xml.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"
#include "io/XmlFile.h"

namespace npcc::license {
namespace {

constexpr std::uint64_t kMaxLicenseBytes = 1ull << 20;   // 许可文件是几 KB 的东西

void Clear(LicenseError* error, std::wstring* detail) {
    if (error != nullptr) {
        *error = LicenseError::None;
    }
    if (detail != nullptr) {
        detail->clear();
    }
}

}  // namespace

std::filesystem::path DefaultLicensePath() {
    return infra::ConfigRoot() / L"license.dat";
}

bool LoadLicenseFile(const std::filesystem::path& path, std::string& bytes,
                     LicenseError* error, std::wstring* detail) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        if (error != nullptr) {
            *error = LicenseError::FileMissing;
        }
        if (detail != nullptr) {
            *detail = L"许可文件不存在：" + path.wstring();
        }
        return false;
    }
    const io::ReadResult read = io::ReadWholeFile(path.wstring(), kMaxLicenseBytes);
    if (!read.ok) {
        if (error != nullptr) {
            *error = LicenseError::IoError;
        }
        if (detail != nullptr) {
            *detail = read.message.empty() ? L"许可文件读不出来" : read.message;
        }
        return false;
    }
    bytes = read.bytes;
    Clear(error, detail);
    return true;
}

bool SaveLicenseFile(const std::filesystem::path& path, std::string_view bytes,
                     LicenseError* error, std::wstring* detail) {
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error != nullptr) {
                *error = LicenseError::IoError;
            }
            if (detail != nullptr) {
                *detail = L"创建许可目录失败：" + path.parent_path().wstring();
            }
            return false;
        }
    }
    const io::WriteResult written = io::WriteFileAtomic(path.wstring(), bytes);
    if (!written.ok) {
        if (error != nullptr) {
            *error = LicenseError::IoError;
        }
        if (detail != nullptr) {
            *detail = written.message.empty() ? io::DescribeWin32Error(written.lastError)
                                              : written.message;
        }
        return false;
    }
    Clear(error, detail);
    return true;
}

std::filesystem::path DefaultTrialStatePath() {
    return infra::ConfigRoot() / L"state.xml";
}

namespace {

// 属性里的十进制秒：空 / 非数字 / 非正数一律当"没记录"。
std::optional<std::int64_t> ParseUtcAttribute(const std::wstring* text) {
    if (text == nullptr || text->empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const long long value = ::wcstoll(text->c_str(), &end, 10);
    if (end == nullptr || end == text->c_str() || *end != L'\0' || value <= 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
}

}  // namespace

bool LoadTrialState(const std::filesystem::path& path, TrialRecord& out, std::wstring* detail) {
    out = TrialRecord{};
    if (detail != nullptr) {
        detail->clear();
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return true;   // 没有记录过：空记录（第一次运行从这一刻开始算）
    }
    infra::XmlDocument document;
    std::wstring parseError;
    if (!io::LoadXmlFile(path.wstring(), document, &parseError)) {
        if (detail != nullptr) {
            *detail = parseError.empty() ? std::wstring(L"state.xml 读不出来") : parseError;
        }
        return false;
    }
    const infra::XmlElement* root = document.Root();
    if (root == nullptr) {
        if (detail != nullptr) {
            *detail = L"state.xml 没有根元素";
        }
        return false;
    }
    const infra::XmlElement* trial = root->Child(L"trial");
    if (trial == nullptr) {
        return true;   // 空壳文件 = 还没写过记录
    }
    out.firstRunUtc = ParseUtcAttribute(trial->Attribute(L"firstRunUtc"));
    out.lastSeenUtc = ParseUtcAttribute(trial->Attribute(L"lastSeenUtc"));
    out.trialNoticeDismissed = trial->AttributeBool(L"trialNoticeDismissed", false);
    out.graceNoticeDismissed = trial->AttributeBool(L"graceNoticeDismissed", false);
    return true;
}

bool SaveTrialState(const std::filesystem::path& path, const TrialRecord& record,
                    std::wstring* detail) {
    infra::XmlDocument document;
    const infra::XmlElementPtr root = document.CreateRoot(L"notepadcc-state");
    const infra::XmlElementPtr trial = root->AddChild(L"trial");
    if (record.firstRunUtc.has_value()) {
        trial->SetAttribute(L"firstRunUtc", std::to_wstring(*record.firstRunUtc));
    }
    if (record.lastSeenUtc.has_value()) {
        trial->SetAttribute(L"lastSeenUtc", std::to_wstring(*record.lastSeenUtc));
    }
    trial->SetAttributeBool(L"trialNoticeDismissed", record.trialNoticeDismissed);
    trial->SetAttributeBool(L"graceNoticeDismissed", record.graceNoticeDismissed);
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::wstring error;
    if (!io::SaveXmlFile(path.wstring(), document, &error)) {
        if (detail != nullptr) {
            *detail = error;
        }
        return false;
    }
    if (detail != nullptr) {
        detail->clear();
    }
    return true;
}

std::optional<std::int64_t> ReadRegistryFirstRun(std::wstring_view subKey) {
    if (subKey.empty()) {
        return std::nullopt;
    }
    const std::wstring sub(subKey);
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(sizeof(value));
    const LONG status = ::RegQueryValueExW(key, L"FirstRun", nullptr, &type,
                                           reinterpret_cast<BYTE*>(&value), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_QWORD ||
        size != static_cast<DWORD>(sizeof(value)) || value <= 0) {
        return std::nullopt;
    }
    return value;
}

bool WriteRegistryFirstRun(std::int64_t value, std::wstring_view subKey) {
    if (subKey.empty() || value <= 0) {
        return false;
    }
    const std::wstring sub(subKey);
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                          &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG status = ::RegSetValueExW(key, L"FirstRun", 0, REG_QWORD,
                                         reinterpret_cast<const BYTE*>(&value),
                                         static_cast<DWORD>(sizeof(value)));
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

TrialRecord MergeTrialRecords(const TrialRecord& file,
                              std::optional<std::int64_t> registryFirstRun) {
    TrialRecord merged = file;
    if (registryFirstRun.has_value() &&
        (!merged.firstRunUtc.has_value() || *registryFirstRun < *merged.firstRunUtc)) {
        merged.firstRunUtc = *registryFirstRun;   // 双写取较早（设计 5.1）
    }
    return merged;
}

}  // namespace npcc::license
