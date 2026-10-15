#pragma once

#include <cstdint>

namespace npcc::edit {

// 大文件分档（设计文档 6.1）。
enum class FileTier : std::uint8_t {
    Normal,   // T1：全量读入，功能完整
    Large,    // T2：内存映射 + ILoader 分块装载，关闭若干吃性能的功能
    Huge,     // T3：只读
};

struct TierPolicy {
    std::uint64_t normalMaxBytes = 8ull * 1024 * 1024;
    std::uint64_t largeMaxBytes = 64ull * 1024 * 1024;
    // 超过这个体积直接拒绝。Scintilla 的文档模型会把内容复制进去，
    // 再大的文件会吃掉数倍内存、装载时间也变得不可接受。
    std::uint64_t refuseAboveBytes = 512ull * 1024 * 1024;

    FileTier Classify(std::uint64_t size) const noexcept {
        if (size <= normalMaxBytes) {
            return FileTier::Normal;
        }
        if (size <= largeMaxBytes) {
            return FileTier::Large;
        }
        return FileTier::Huge;
    }

    bool Acceptable(std::uint64_t size) const noexcept {
        return size <= refuseAboveBytes;
    }
};

inline const wchar_t* TierName(FileTier tier) noexcept {
    switch (tier) {
        case FileTier::Large: return L"大文件模式";
        case FileTier::Huge:  return L"超大文件（只读）";
        default:              return L"";
    }
}

}  // namespace npcc::edit
