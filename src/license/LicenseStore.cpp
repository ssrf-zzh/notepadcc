#include "license/LicenseStore.h"

#include <cstdint>
#include <system_error>

#include "infra/Paths.h"
#include "io/AtomicFile.h"
#include "io/FileReader.h"

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

}  // namespace npcc::license