#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "license/LicenseTypes.h"

namespace npcc::license {

struct LicenseVerifyResult {
    LicenseError error = LicenseError::None;
    std::wstring detail;   // 具体原因（哪个字段、什么值、解析位置），给日志与对话框
    LicenseInfo  info;

    bool ok() const { return error == LicenseError::None; }
};

// 容器解析 + 验签 + 字段校验（设计 3.1 的顺序）：
//   读容器 → Base64 解出 payload 原始字节 → 用容器里的 keyId 选公钥先验签 →
//   验签通过**之后**才解析字段。
// 先验签后解析，解析器就永远见不到未验证的字节；顺序反过来等于"拿未验证的
// 内容决定下一步"（与更新清单同一条纪律）。
//
// nowUnix 显式传入：过期（Expired）与未生效（NotYetValid）的判定要能被单测
// 钉在固定时刻上。注意这两种情况会返回 `error` 非 None、`info` 却是填好的——
// 界面要能说清楚"谁的许可、什么时候到期"。
LicenseVerifyResult VerifyLicenseBytes(std::string_view containerXml, std::int64_t nowUnix);

// 签发端（tools/licgen，B12）与验证端共用同一套序列化：把许可信息编成 payload
// 字节——键按字典序、行尾 LF、UTF-8 无 BOM（设计 3.3 的"两套代码共用一个实现，
// 避免两端格式漂移"）；缺失的可选字段整行不写，必填字段缺失时照写缺失的样子
// （验证端会报"缺必填键"）。
std::string SerializeLicensePayload(const LicenseInfo& info);

}  // namespace npcc::license