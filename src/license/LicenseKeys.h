// 许可验签的内置公钥表（设计《notepadcc许可与激活设计.md》3.4）。由
// tools/make_license_fixtures.ps1 生成（开发钥匙）；正式发布前用离线生成的
// 真钥匙替换这里、并同步重跑 tools/make_license_fixtures.ps1 更新测试固定件。
// 至少能同时装两把（当前 + 下一把），换钥期间旧许可仍可用。
#pragma once

#include <string_view>

namespace npcc::license {

struct LicenseKeyEntry {
    std::string_view keyId;
    std::string_view publicKeyBase64;   // BCrypt RSAPUBLIC_BLOB
};

inline constexpr LicenseKeyEntry kLicenseKeys[] = {
    {"lic-2026a", "UlNBMQAIAAADAAAAAAEAAAAAAAAAAAAAAQAB3mk4CIklDmBqCUfhnRw9qUEWr5/fy3Y46+GQsVPE+UyeFnP1O0Vaa+20l+l7mqd2htRZYs9suVL+eczWRUgHf1BsJsUZi9IL957zS8D5zJ57yEyoGO5y+WpPpnAXg/bnYb1i7hEc9QAK6QC2ZLfUK0ab4pRN9mQcpuzuX8EpIMl7Oc1UhNGF+JWevvoMl12ESLjBEWyTCz52Zf0vbnNICw0qXBOcDCPjBONEB8cJT3txQQwVjA54KO8pTuctT1/KT8D55+sMP37ZXkr9+v5RkIPpBNSAV0f087oNz5fWAwbfjO9mbgARAxJf+T5xlogf/s1HfH2LeLGJnM53bm52NQ=="},
    {"lic-2027a", "UlNBMQAIAAADAAAAAAEAAAAAAAAAAAAAAQAB7K/xbzKOy4omwlDgauf+FYhG+NsiFdehrrq7rhIwb/qcoXp/flf+2mlz1iKD97XpWsobUgZpIeqmMDnhg/S6po3YLkFkuaukhIUASFgx4HMSIwoRU/ZRFN+BEulC7cSIjrIHXkDzU3BDWBBn6V3qyIubVk3ciKNiytP3faaxx4A0Qev3aj9Pg/QfpKz49jpxKpZMfsMhcFp7aZZvlCLzuGEz4y5/UdWB5s3DaEbEURDmAGkRvPCG8jN0mlaVLJOyzQInLXidm7W5LBn+C8N6gbPrbRFLg46gTNKsvcEUHoVuFYsX0yg7pqx+r6Sw61e7eg4NVK7PNtE1SH2bdYpPdQ=="},
};

// 找不到返回空 —— 调用方按 KeyUnknown 处理（提示升级版本，见设计 9）
inline std::string_view FindLicensePublicKey(std::string_view keyId) {
    for (const LicenseKeyEntry& entry : kLicenseKeys) {
        if (entry.keyId == keyId) {
            return entry.publicKeyBase64;
        }
    }
    return {};
}

}  // namespace npcc::license

