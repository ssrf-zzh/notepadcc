#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "license/LicenseTypes.h"

namespace npcc::license {

// 机器指纹与短码（B4，《notepadcc许可与激活设计.md》4.1–4.3）。
//
// 短码是"能在电话或微信里念清楚的机器标识"：四个采集项各自摘出 5 个字符，
// 四组以 '-' 连成 XXXXX-XXXXX-XXXXX-XXXXX。它只用于**比对**，不是密钥；
// 计算全在本地，只有用户主动点"在线激活"时才随请求离开本机（4.3）。
//
// 与设计文档 4.1 的一处落地改判（写在这里，免得后来人照着"四项拼接后取前
// 100 位"重做一遍）：4.2 的宽容策略要能回答"MachineGuid 这一项中没中""一共
// 中了几项"，而"整串哈希再取前 100 位"只能回答全等 / 不等。因此改成**每个
// 采集项单独取 SHA-256 的前 25 位**（5 个字符）：拼起来同样是 100 位、短码
// 外观与长度完全不变，但每一项是否匹配可以逐组比对。

enum class FingerprintItem {
    MachineGuid = 0,   // HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
    VolumeSerial = 1,  // 系统盘卷序列号（GetVolumeInformationW）
    CpuBrand = 2,      // CPU 品牌串（CPUID 0x80000002..0x80000004）
    BoardUuid = 3,     // 主板 UUID（SMBIOS Type 1；读不到就留空）
};

constexpr int kFingerprintItemCount = 4;

// 采集项的名字（日志与调试输出用；界面文案走语言包，不从这里取）
std::string_view FingerprintItemName(FingerprintItem item);

// 一台机器的四项采集值。空串 = 这一项在本机读不到——读不到不是错误，只是
// 该项不参与判定（判定逻辑见 Match）。下标顺序就是 FingerprintItem 的值，
// 也就是短码分组的顺序，永不重排。
struct Fingerprint {
    std::array<std::string, kFingerprintItemCount> values;

    bool Has(FingerprintItem item) const;
    int PresentCount() const;   // 有几项读得到
};

// 采集接口（可注入）：真实机器走 Current()，单元测试用桩——判定逻辑因此
// 不必真的去读注册表、卷序列号与 SMBIOS。
struct FingerprintProvider {
    std::function<bool(std::string& out)> machineGuid;
    std::function<bool(std::string& out)> volumeSerial;
    std::function<bool(std::string& out)> cpuBrand;
    std::function<bool(std::string& out)> boardUuid;
};

Fingerprint Collect(const FingerprintProvider& provider);
Fingerprint Current();   // 读本机：注册表 + 卷序列号 + CPUID + SMBIOS

// 短码字母表：Base32 去掉念不清、抄不准的 0 O 1 I，剩 8 个数字 + 24 个字母
// = 32 个符号，正好每符号 5 位（附录 B 的"授权码"同用这套字母表）。
constexpr std::string_view kFingerprintAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

// 读不到的采集项，分组固定写成 00000。字母表里没有 '0'，所以它不可能是任何
// 真实摘要的取值——两端看见它都知道"这一项当时没读到"。
constexpr std::string_view kFingerprintUnknownGroup = "00000";

// 一个采集值 → 5 个字符（空值 → 00000）。先归一化（去首尾空白 + 转大写），
// 否则 CPU 品牌串的填充空格、MachineGuid 的大小写会让同一台机器算出两个短码。
std::string FingerprintGroup(std::string_view value);

// 四项 → 短码 "XXXXX-XXXXX-XXXXX-XXXXX"
std::string Format(const Fingerprint& fingerprint);

// 用户输入的归一化：去掉 '-'、空格、下划线，转大写。**不猜**易混字符——
// 0 O 1 I 不在字母表里，出现即是输入错了，交给 IsWellFormedCode 拒绝，而不是
// 悄悄替换成别的字符（替换错了等于把许可指向另一台机器）。
std::string NormalizeCode(std::string_view text);

// 形态校验（归一化之后）：20 个字符，每组 5 个符号要么全在字母表里、要么是 00000。
bool IsWellFormedCode(std::string_view code);

// 宽容匹配的判定（设计 4.2）
enum class FingerprintVerdict {
    NotBound,      // fingerprintMode=none：站点许可，不做机器判定
    Match,         // MachineGuid 组相同（exact 模式下是短码完全一致）
    Tolerated,     // MachineGuid 不中但至少两项中：硬件小幅更换，放行 + 一次性提示
    Mismatch,      // 至多一项中：不是这台机器
    Unavailable,   // 本机读得到的采集项不足两项，宽容判定无从谈起
    Malformed,     // 许可里的短码本身不合法
};

struct FingerprintMatch {
    FingerprintVerdict verdict = FingerprintVerdict::Mismatch;
    int matchedGroups = 0;   // 相同、且双方都读得到的组数
    int unknownGroups = 0;   // 一方读不到、无法比对的组数

    bool Allows() const;     // Match / Tolerated / NotBound
};

// machine：本机指纹；licenseCode：许可里的短码；mode：payload 的 fingerprintMode。
FingerprintMatch Match(const Fingerprint& machine, std::string_view licenseCode,
                       FingerprintMode mode);

}  // namespace npcc::license
