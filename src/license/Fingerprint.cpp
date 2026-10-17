#include "license/Fingerprint.h"

#include <windows.h>

#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "infra/Crypto.h"
#include "infra/TextCodec.h"

namespace npcc::license {
namespace {

// 归一化：去掉首尾空白、ASCII 转大写。CPU 品牌串尾部带填充空格，MachineGuid
// 在不同 Windows 上的大小写也不总一致——不归一，同一台机器会算出两个短码。
std::string NormalizeItemValue(std::string_view value) {
    const auto isSpace = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && isSpace(value[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(value[end - 1])) {
        --end;
    }
    std::string out(value.substr(begin, end - begin));
    for (char& ch : out) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return out;
}

// ----- Windows 采集（四项各自独立，任何一项失败都不影响其余三项）-----

bool ReadMachineGuid(std::string& out) {
    wchar_t buffer[128] = {};
    DWORD size = sizeof(buffer);
    const LSTATUS status =
        ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                       L"MachineGuid", RRF_RT_REG_SZ, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    out = infra::Utf8FromWide(buffer);
    return !out.empty();
}

bool ReadVolumeSerial(std::string& out) {
    wchar_t systemDir[MAX_PATH] = {};
    if (::GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
        return false;
    }
    const std::wstring root = std::wstring(1, systemDir[0]) + L":\\";
    DWORD serial = 0;
    if (!::GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr,
                                 nullptr, 0)) {
        return false;
    }
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%08X",
                  static_cast<unsigned int>(serial));    // 定长 8 位十六进制
    out = text;
    return true;
}

bool ReadCpuBrand(std::string& out) {
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned int>(regs[0]) < 0x80000004u) {
        return false;   // 这颗 CPU 没有品牌串叶子
    }
    char brand[49] = {};
    for (int leaf = 0; leaf < 3; ++leaf) {
        __cpuid(regs, 0x80000002 + leaf);
        std::memcpy(brand + leaf * 16, regs, 16);
    }
    out = brand;   // 尾部填充空格交给归一化
    return true;
}

// SMBIOS Type 1 的 UUID 在结构体偏移 08h、16 字节：前三个字段按小端存放，
// 后 8 字节按原序（SMBIOS 规范）。全 0x00 与全 0xFF 一律当"没有"——虚拟机
// 与不少主板就是这么写的，把它们当成有效 UUID 等于给一批机器发同一个码。
//
// 读这张表踩过两个坑（真机上才暴露，测试桩永远看不见）：
// ① GetSystemFirmwareTable 的 RSMB 签名是**大端序**字面量（'R' 在高字节，
//    0x52534D42）。按小端拼出的 0x424D5352 只会返回 0，静默变成"读不到"；
// ② RSMB 返回的缓冲区**前面还有 8 字节头**（RAW_SMBIOS_DATA：Used20Method、
//    SMBIOS 主/次版本、DMI 版本、表长），结构链从偏移 8 开始。把那 8 个字节
//    当结构头解析，第一步就"长度非法"。
bool ReadBoardUuid(std::string& out) {
    const DWORD signature = (static_cast<DWORD>('R') << 24) | (static_cast<DWORD>('S') << 16) |
                            (static_cast<DWORD>('M') << 8) | static_cast<DWORD>('B');
    const DWORD size = ::GetSystemFirmwareTable(signature, 0, nullptr, 0);
    if (size < 16 || size > (1u << 20)) {
        return false;   // 正常是几十 KB；拿不到大小就别硬来
    }
    std::vector<unsigned char> buffer(size, 0);
    const DWORD got = ::GetSystemFirmwareTable(signature, 0, buffer.data(), size);
    if (got == 0 || got > size) {
        return false;
    }
    const std::size_t tableLength = static_cast<std::size_t>(buffer[4]) |
                                    (static_cast<std::size_t>(buffer[5]) << 8) |
                                    (static_cast<std::size_t>(buffer[6]) << 16) |
                                    (static_cast<std::size_t>(buffer[7]) << 24);
    const std::size_t end = 8 + tableLength;
    if (tableLength < 8 || end > got) {
        return false;   // 头里的表长与缓冲区对不上：不信它
    }

    std::size_t offset = 8;   // 跳过 RAW_SMBIOS_DATA 头
    while (offset + 4 <= end) {
        const unsigned char type = buffer[offset];
        const unsigned char length = buffer[offset + 1];
        if (length < 4 || offset + length > end) {
            break;
        }
        if (type == 127) {   // End-of-table
            break;
        }
        if (type == 1) {
            if (length < 0x18) {
                return false;   // 结构体里没有 UUID 字段
            }
            const unsigned char* uuid = buffer.data() + offset + 0x08;
            bool allZero = true;
            bool allOnes = true;
            for (int index = 0; index < 16; ++index) {
                allZero = allZero && uuid[index] == 0x00;
                allOnes = allOnes && uuid[index] == 0xFF;
            }
            if (allZero || allOnes) {
                return false;
            }
            char text[40] = {};
            std::snprintf(text, sizeof(text),
                          "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
                          "%02X%02X%02X%02X%02X%02X",
                          uuid[3], uuid[2], uuid[1], uuid[0], uuid[5], uuid[4], uuid[7],
                          uuid[6], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
                          uuid[14], uuid[15]);
            out = text;
            return true;
        }
        // 跳到下一个结构：先走完字符串区（以双 NUL 结束）
        std::size_t next = offset + length;
        while (next + 1 < end && !(buffer[next] == 0 && buffer[next + 1] == 0)) {
            ++next;
        }
        next += 2;
        if (next <= offset) {
            break;
        }
        offset = next;
    }
    return false;
}

}  // namespace

std::string_view FingerprintItemName(FingerprintItem item) {
    switch (item) {
        case FingerprintItem::MachineGuid: return "machineguid";
        case FingerprintItem::VolumeSerial: return "volumeserial";
        case FingerprintItem::CpuBrand: return "cpubrand";
        case FingerprintItem::BoardUuid: return "boarduuid";
    }
    return "unknown";
}

bool Fingerprint::Has(FingerprintItem item) const {
    return !values[static_cast<int>(item)].empty();
}

int Fingerprint::PresentCount() const {
    int count = 0;
    for (const std::string& value : values) {
        if (!value.empty()) {
            ++count;
        }
    }
    return count;
}

Fingerprint Collect(const FingerprintProvider& provider) {
    Fingerprint fingerprint;
    const std::function<bool(std::string&)>* sources[kFingerprintItemCount] = {
        &provider.machineGuid, &provider.volumeSerial, &provider.cpuBrand,
        &provider.boardUuid,
    };
    for (int index = 0; index < kFingerprintItemCount; ++index) {
        std::string value;
        if (*sources[index] && (*sources[index])(value)) {
            fingerprint.values[index] = NormalizeItemValue(value);
        }
    }
    return fingerprint;
}

Fingerprint Current() {
    return Collect(FingerprintProvider{ReadMachineGuid, ReadVolumeSerial, ReadCpuBrand,
                                       ReadBoardUuid});
}

std::string FingerprintGroup(std::string_view value) {
    const std::string normalized = NormalizeItemValue(value);
    if (normalized.empty()) {
        return std::string(kFingerprintUnknownGroup);
    }
    const infra::Sha256Digest digest = infra::Sha256(normalized.data(), normalized.size());
    // 前 25 位：bit 0 是摘要第一个字节的最高位 -> 5 个符号、每符号 5 位
    const std::uint32_t window = (static_cast<std::uint32_t>(digest.bytes[0]) << 24) |
                                 (static_cast<std::uint32_t>(digest.bytes[1]) << 16) |
                                 (static_cast<std::uint32_t>(digest.bytes[2]) << 8) |
                                 static_cast<std::uint32_t>(digest.bytes[3]);
    const std::uint32_t bits = window >> 7;
    std::string group(5, ' ');
    for (int index = 0; index < 5; ++index) {
        group[static_cast<std::size_t>(index)] =
            kFingerprintAlphabet[(bits >> (5 * (4 - index))) & 31u];
    }
    return group;
}

std::string Format(const Fingerprint& fingerprint) {
    std::string code;
    for (int index = 0; index < kFingerprintItemCount; ++index) {
        if (index != 0) {
            code.push_back('-');
        }
        code += FingerprintGroup(fingerprint.values[static_cast<std::size_t>(index)]);
    }
    return code;
}

std::string NormalizeCode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (ch == '-' || ch == ' ' || ch == '\t' || ch == '_' || ch == '\r' || ch == '\n') {
            continue;
        }
        out.push_back((ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch);
    }
    return out;
}

bool IsWellFormedCode(std::string_view code) {
    const std::string normalized = NormalizeCode(code);
    if (normalized.size() != 20) {
        return false;
    }
    for (int group = 0; group < kFingerprintItemCount; ++group) {
        const std::string_view piece(normalized.data() + group * 5, 5);
        if (piece == kFingerprintUnknownGroup) {
            continue;
        }
        for (const char ch : piece) {
            if (kFingerprintAlphabet.find(ch) == std::string_view::npos) {
                return false;
            }
        }
    }
    return true;
}

bool FingerprintMatch::Allows() const {
    return verdict == FingerprintVerdict::Match || verdict == FingerprintVerdict::Tolerated ||
           verdict == FingerprintVerdict::NotBound;
}

FingerprintMatch Match(const Fingerprint& machine, std::string_view licenseCode,
                       FingerprintMode mode) {
    FingerprintMatch result;
    if (mode == FingerprintMode::None) {
        result.verdict = FingerprintVerdict::NotBound;
        return result;
    }
    const std::string code = NormalizeCode(licenseCode);
    if (!IsWellFormedCode(code)) {
        result.verdict = FingerprintVerdict::Malformed;
        return result;
    }

    bool same[kFingerprintItemCount] = {};      // 双方都读得到、且相同
    bool unknown[kFingerprintItemCount] = {};   // 只有一方读得到，无从比对
    for (int index = 0; index < kFingerprintItemCount; ++index) {
        const std::string group =
            FingerprintGroup(machine.values[static_cast<std::size_t>(index)]);
        const std::string_view licensed(code.data() + index * 5, 5);
        if (group == kFingerprintUnknownGroup || licensed == kFingerprintUnknownGroup) {
            // 双方都读不到：无从比对也不构成 unknown；只有一方读不到：算 unknown
            unknown[index] = (group != licensed);
            continue;
        }
        same[index] = (group == licensed);
        if (same[index]) {
            ++result.matchedGroups;
        }
    }
    for (const bool flag : unknown) {
        if (flag) {
            ++result.unknownGroups;
        }
    }

    if (mode == FingerprintMode::Exact) {
        // 严格模式：短码必须与本机完全一致（含"双方都读不到"的那些项）。
        // 两边都按归一化形态比：code 早已归一（没有连字符），本机这边也要归一。
        result.verdict = (NormalizeCode(Format(machine)) == code) ? FingerprintVerdict::Match
                                                                  : FingerprintVerdict::Mismatch;
        return result;
    }

    // 宽容模式（设计 4.2）：MachineGuid 中即放行
    if (same[0]) {
        result.verdict = FingerprintVerdict::Match;
        return result;
    }
    if (machine.PresentCount() < 2) {
        result.verdict = FingerprintVerdict::Unavailable;
        return result;
    }
    result.verdict = (result.matchedGroups >= 2) ? FingerprintVerdict::Tolerated
                                                 : FingerprintVerdict::Mismatch;
    return result;
}

}  // namespace npcc::license
